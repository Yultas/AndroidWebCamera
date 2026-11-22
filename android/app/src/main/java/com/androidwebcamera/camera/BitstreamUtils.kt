package com.androidwebcamera.camera

import com.androidwebcamera.model.StreamCodec
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Pure JVM bitstream utilities for H.264 Annex-B, H.265 (HEVC) Annex-B, and AV1 OBU/IVF.
 * Zero Android framework dependencies so every parser and muxer is directly unit-testable.
 */
object BitstreamUtils {

    private val ANNEX_B_START_CODE = byteArrayOf(0, 0, 0, 1)

    // H.264 NAL unit types
    const val H264_NAL_IDR = 5
    const val H264_NAL_SPS = 7
    const val H264_NAL_PPS = 8

    // H.265 (HEVC) NAL unit types
    const val HEVC_NAL_IDR_W_RADL = 19
    const val HEVC_NAL_IDR_N_LP = 20
    const val HEVC_NAL_CRA_NUT = 21
    const val HEVC_NAL_VPS = 32
    const val HEVC_NAL_SPS = 33
    const val HEVC_NAL_PPS = 34

    // AV1 OBU types
    const val AV1_OBU_SEQUENCE_HEADER = 1
    const val AV1_OBU_TEMPORAL_DELIMITER = 2
    const val AV1_OBU_FRAME_HEADER = 3
    const val AV1_OBU_FRAME = 6

    data class Av1Obu(
        val type: Int,
        val rawBytes: ByteArray
    )

    /** Splits an Annex-B stream into raw NAL units (without start codes). */
    fun splitAnnexBNals(data: ByteArray): List<ByteArray> {
        val starts = mutableListOf<Pair<Int, Int>>() // (offset, startCodeLen)
        var i = 0
        while (i + 2 < data.size) {
            if (data[i] == 0.toByte() && data[i + 1] == 0.toByte()) {
                if (data[i + 2] == 1.toByte()) {
                    starts.add(i to 3)
                    i += 3
                    continue
                }
                if (i + 3 < data.size && data[i + 2] == 0.toByte() && data[i + 3] == 1.toByte()) {
                    starts.add(i to 4)
                    i += 4
                    continue
                }
            }
            i++
        }
        if (starts.isEmpty()) return emptyList()

        val result = ArrayList<ByteArray>(starts.size)
        for (idx in starts.indices) {
            val nalStart = starts[idx].first + starts[idx].second
            val nalEnd = if (idx + 1 < starts.size) starts[idx + 1].first else data.size
            if (nalEnd > nalStart) {
                result.add(data.copyOfRange(nalStart, nalEnd))
            }
        }
        return result
    }

    /** Wraps raw NAL units with 4-byte Annex-B start codes (`00 00 00 01`). */
    fun joinAnnexBNals(nals: List<ByteArray>): ByteArray {
        val out = ByteArrayOutputStream()
        for (nal in nals) {
            if (nal.isEmpty()) continue
            out.write(ANNEX_B_START_CODE)
            out.write(nal)
        }
        return out.toByteArray()
    }

    fun h264NalType(nal: ByteArray): Int =
        if (nal.isNotEmpty()) nal[0].toInt() and 0x1F else -1

    fun hevcNalType(nal: ByteArray): Int =
        if (nal.isNotEmpty()) (nal[0].toInt() ushr 1) and 0x3F else -1

    fun isH264KeyFrame(data: ByteArray): Boolean =
        splitAnnexBNals(data).any { h264NalType(it) == H264_NAL_IDR }

    fun isHevcKeyFrame(data: ByteArray): Boolean =
        splitAnnexBNals(data).any {
            val t = hevcNalType(it)
            t == HEVC_NAL_IDR_W_RADL || t == HEVC_NAL_IDR_N_LP || t == HEVC_NAL_CRA_NUT
        }

    fun hasH264ParameterSets(data: ByteArray): Boolean {
        val types = splitAnnexBNals(data).map { h264NalType(it) }
        return H264_NAL_SPS in types && H264_NAL_PPS in types
    }

    fun hasHevcParameterSets(data: ByteArray): Boolean {
        val types = splitAnnexBNals(data).map { hevcNalType(it) }
        return HEVC_NAL_VPS in types && HEVC_NAL_SPS in types && HEVC_NAL_PPS in types
    }

    /**
     * Parses raw AV1 OBU units from a temporal unit or config buffer.
     * Supports OBUs with `obu_has_size_field = 1` (standard for MediaCodec / IVF / MP4)
     * as well as trailing OBUs without size field.
     */
    fun parseAv1Obus(data: ByteArray, startOffset: Int = 0): List<Av1Obu> {
        val result = mutableListOf<Av1Obu>()
        var pos = startOffset
        while (pos < data.size) {
            val obuStart = pos
            val header = data[pos].toInt() and 0xFF
            if ((header and 0x80) != 0) {
                // obu_forbidden_bit must be 0
                break
            }
            val obuType = (header ushr 3) and 0x0F
            val hasExtension = (header and 0x04) != 0
            val hasSize = (header and 0x02) != 0
            pos++

            if (hasExtension) {
                if (pos >= data.size) break
                pos++
            }

            val payloadSize: Int
            if (hasSize) {
                val (sizeVal, bytesRead) = readLeb128(data, pos) ?: break
                if (sizeVal < 0 || sizeVal > data.size - (pos + bytesRead)) break
                pos += bytesRead
                payloadSize = sizeVal
            } else {
                payloadSize = data.size - pos
            }

            val obuEnd = pos + payloadSize
            if (obuEnd > data.size) break
            result.add(Av1Obu(type = obuType, rawBytes = data.copyOfRange(obuStart, obuEnd)))
            pos = obuEnd
        }
        return result
    }

    /**
     * Extracts the raw `OBU_SEQUENCE_HEADER` from either:
     * - a raw AV1 OBU bitstream / temporal unit, OR
     * - an ISO BMFF `av1C` decoder configuration record (4-byte header with marker=1, version=1 -> 0x81).
     */
    fun extractAv1SequenceHeaderObu(data: ByteArray): ByteArray? {
        if (data.isEmpty()) return null

        // Try direct OBU stream first
        parseAv1Obus(data, 0).firstOrNull { it.type == AV1_OBU_SEQUENCE_HEADER }?.let {
            return it.rawBytes
        }

        // Fallback: Android MediaFormat csd-0 for AV1 can be an av1C record (starts with 0x81, 4-byte header)
        if (data.size > 4 && (data[0].toInt() and 0xFF) == 0x81) {
            parseAv1Obus(data, 4).firstOrNull { it.type == AV1_OBU_SEQUENCE_HEADER }?.let {
                return it.rawBytes
            }
        }
        return null
    }

    fun hasAv1SequenceHeader(data: ByteArray): Boolean =
        parseAv1Obus(data, 0).any { it.type == AV1_OBU_SEQUENCE_HEADER }

    private fun readLeb128(data: ByteArray, offset: Int): Pair<Int, Int>? {
        var value = 0L
        var bytesRead = 0
        for (i in 0 until 8) {
            if (offset + i >= data.size) return null
            val byte = data[offset + i].toInt() and 0xFF
            value = value or ((byte and 0x7F).toLong() shl (i * 7))
            bytesRead = i + 1
            if ((byte and 0x80) == 0) {
                if (value > Int.MAX_VALUE) return null
                return value.toInt() to bytesRead
            }
        }
        return null
    }

    /**
     * Builds a 32-byte IVF file header for streaming raw AV1 frames to FFmpeg / OBS / VLC.
     * Uses microsecond timebase (1 / 1,000,000) so MediaCodec PTS maps 1:1.
     */
    fun buildIvfFileHeader(width: Int, height: Int, timebaseDenom: Int = 1_000_000): ByteArray {
        return ByteBuffer.allocate(32).order(ByteOrder.LITTLE_ENDIAN).apply {
            put('D'.code.toByte())
            put('K'.code.toByte())
            put('I'.code.toByte())
            put('F'.code.toByte())
            putShort(0) // version
            putShort(32) // header length
            put('A'.code.toByte())
            put('V'.code.toByte())
            put('0'.code.toByte())
            put('1'.code.toByte())
            putShort(width.coerceIn(1, 65535).toShort())
            putShort(height.coerceIn(1, 65535).toShort())
            putInt(timebaseDenom.coerceAtLeast(1)) // frame rate / timebase denominator
            putInt(1) // timebase numerator
            putInt(0) // num frames (0 = live / unknown)
            putInt(0) // unused
        }.array()
    }

    /** Builds a 12-byte IVF frame header preceding each AV1 temporal unit. */
    fun buildIvfFrameHeader(frameSize: Int, pts: Long): ByteArray {
        return ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN).apply {
            putInt(frameSize)
            putLong(pts.coerceAtLeast(0L))
        }.array()
    }

    /**
     * Stateful accumulator for codec parameter sets (H.264 SPS+PPS, H.265 VPS+SPS+PPS, AV1 SeqHdr).
     * Safely merges split CODEC_CONFIG buffers (common on OMX/HiSilicon encoders) and ensures
     * keyframes always have configuration headers prepended when needed.
     */
    class CodecConfigAccumulator(private val codec: StreamCodec) {
        private var h264Sps: ByteArray? = null
        private var h264Pps: ByteArray? = null

        private var hevcVps: ByteArray? = null
        private var hevcSps: ByteArray? = null
        private var hevcPps: ByteArray? = null

        private var av1SeqHeader: ByteArray? = null

        /** Ingests bytes from `csd-0`/`csd-1`, `BUFFER_FLAG_CODEC_CONFIG`, or a keyframe buffer. */
        fun ingest(data: ByteArray): ByteArray? {
            if (data.isEmpty()) return currentConfigBytes()
            when (codec) {
                StreamCodec.H264 -> {
                    for (nal in splitAnnexBNals(data)) {
                        when (h264NalType(nal)) {
                            H264_NAL_SPS -> h264Sps = nal
                            H264_NAL_PPS -> h264Pps = nal
                        }
                    }
                }

                StreamCodec.H265 -> {
                    for (nal in splitAnnexBNals(data)) {
                        when (hevcNalType(nal)) {
                            HEVC_NAL_VPS -> hevcVps = nal
                            HEVC_NAL_SPS -> hevcSps = nal
                            HEVC_NAL_PPS -> hevcPps = nal
                        }
                    }
                }

                StreamCodec.AV1 -> {
                    extractAv1SequenceHeaderObu(data)?.let { av1SeqHeader = it }
                }

                StreamCodec.MJPEG -> Unit
            }
            return currentConfigBytes()
        }

        /** Returns the complete combined config payload if all required parameter sets are available. */
        fun currentConfigBytes(): ByteArray? = when (codec) {
            StreamCodec.H264 -> {
                val sps = h264Sps
                val pps = h264Pps
                if (sps != null && pps != null) joinAnnexBNals(listOf(sps, pps)) else null
            }

            StreamCodec.H265 -> {
                val vps = hevcVps
                val sps = hevcSps
                val pps = hevcPps
                if (vps != null && sps != null && pps != null) {
                    joinAnnexBNals(listOf(vps, sps, pps))
                } else {
                    null
                }
            }

            StreamCodec.AV1 -> av1SeqHeader?.copyOf()
            StreamCodec.MJPEG -> null
        }

        /** Ensures a keyframe carries its codec configuration header if the encoder omitted it. */
        fun ensureConfigOnKeyFrame(payload: ByteArray): ByteArray {
            ingest(payload)
            val cfg = currentConfigBytes() ?: return payload
            return when (codec) {
                StreamCodec.H264 -> if (hasH264ParameterSets(payload)) payload else cfg + payload
                StreamCodec.H265 -> if (hasHevcParameterSets(payload)) payload else cfg + payload
                StreamCodec.AV1 -> if (hasAv1SequenceHeader(payload)) payload else cfg + payload
                StreamCodec.MJPEG -> payload
            }
        }
    }
}
