package com.androidwebcamera.camera

import com.androidwebcamera.model.StreamCodec
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class BitstreamUtilsTest {

    @Test
    fun splitAnnexBHandles3ByteAnd4ByteStartCodes() {
        val stream = byteArrayOf(
            0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1F, // 4-byte SPS
            0, 0, 1, 0x68, 0xCE.toByte(), 0x3C, // 3-byte PPS
            0, 0, 0, 1, 0x65, 0x88.toByte()     // 4-byte IDR
        )
        val nals = BitstreamUtils.splitAnnexBNals(stream)
        assertEquals(3, nals.size)
        assertEquals(BitstreamUtils.H264_NAL_SPS, BitstreamUtils.h264NalType(nals[0]))
        assertEquals(BitstreamUtils.H264_NAL_PPS, BitstreamUtils.h264NalType(nals[1]))
        assertEquals(BitstreamUtils.H264_NAL_IDR, BitstreamUtils.h264NalType(nals[2]))
        assertTrue(BitstreamUtils.isH264KeyFrame(stream))
        assertTrue(BitstreamUtils.hasH264ParameterSets(stream))
    }

    @Test
    fun h264AccumulatorMergesSplitSpsAndPpsConfigBuffers() {
        val acc = BitstreamUtils.CodecConfigAccumulator(StreamCodec.H264)
        val spsOnly = byteArrayOf(0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1F)
        val ppsOnly = byteArrayOf(0, 0, 0, 1, 0x68, 0xCE.toByte(), 0x3C)
        val idrOnly = byteArrayOf(0, 0, 0, 1, 0x65, 0x88.toByte(), 0x84.toByte())

        // Split config buffers (common on OMX/HiSilicon) must not overwrite SPS with PPS
        assertNull(acc.ingest(spsOnly))
        val merged = acc.ingest(ppsOnly)
        assertNotNull(merged)
        assertTrue(BitstreamUtils.hasH264ParameterSets(merged!!))

        val keyWithCfg = acc.ensureConfigOnKeyFrame(idrOnly)
        assertTrue(BitstreamUtils.hasH264ParameterSets(keyWithCfg))
        assertTrue(BitstreamUtils.isH264KeyFrame(keyWithCfg))
        assertEquals(merged.size + idrOnly.size, keyWithCfg.size)

        // Keyframe that already has SPS+PPS should not get duplicated headers
        val alreadyComplete = acc.ensureConfigOnKeyFrame(keyWithCfg)
        assertArrayEquals(keyWithCfg, alreadyComplete)
    }

    @Test
    fun hevcAccumulatorMergesVpsSpsPpsAndDetectsIrapKeyframes() {
        val acc = BitstreamUtils.CodecConfigAccumulator(StreamCodec.H265)
        // HEVC NAL header: type in bits [6:1] -> (type << 1)
        val vps = byteArrayOf(0, 0, 0, 1, (32 shl 1).toByte(), 0x01, 0x0C, 0x01)
        val sps = byteArrayOf(0, 0, 0, 1, (33 shl 1).toByte(), 0x01, 0x01, 0x60)
        val pps = byteArrayOf(0, 0, 0, 1, (34 shl 1).toByte(), 0x01, 0xC1.toByte(), 0x73)
        val idrW = byteArrayOf(0, 0, 0, 1, (19 shl 1).toByte(), 0x01, 0xAF.toByte())
        val cra = byteArrayOf(0, 0, 0, 1, (21 shl 1).toByte(), 0x01, 0xB0.toByte())
        val trailR = byteArrayOf(0, 0, 0, 1, (1 shl 1).toByte(), 0x01, 0x02)

        assertTrue(BitstreamUtils.isHevcKeyFrame(idrW))
        assertTrue(BitstreamUtils.isHevcKeyFrame(cra))
        assertFalse(BitstreamUtils.isHevcKeyFrame(trailR))

        assertNull(acc.ingest(vps))
        assertNull(acc.ingest(sps))
        val combined = acc.ingest(pps)
        assertNotNull(combined)
        assertTrue(BitstreamUtils.hasHevcParameterSets(combined!!))

        val prepared = acc.ensureConfigOnKeyFrame(idrW)
        assertTrue(BitstreamUtils.hasHevcParameterSets(prepared))
        assertTrue(BitstreamUtils.isHevcKeyFrame(prepared))
        assertEquals(combined.size + idrW.size, prepared.size)
    }

    @Test
    fun av1ObuParsingExtractsSequenceHeaderFromRawAndAv1cCsd() {
        // OBU_SEQUENCE_HEADER: forbidden=0, type=1 (0x08), has_size=1 (0x02) => 0x0A, size=4, 4 payload bytes
        val seqObu = byteArrayOf(0x0A, 0x04, 0x00, 0x00, 0x00, 0x42)
        // OBU_FRAME: type=6 (0x30), has_size=1 (0x02) => 0x32, size=3, 3 payload bytes
        val frameObu = byteArrayOf(0x32, 0x03, 0x10, 0x20, 0x30)

        val temporalUnit = seqObu + frameObu
        val obus = BitstreamUtils.parseAv1Obus(temporalUnit)
        assertEquals(2, obus.size)
        assertEquals(BitstreamUtils.AV1_OBU_SEQUENCE_HEADER, obus[0].type)
        assertArrayEquals(seqObu, obus[0].rawBytes)
        assertEquals(BitstreamUtils.AV1_OBU_FRAME, obus[1].type)
        assertArrayEquals(frameObu, obus[1].rawBytes)
        assertTrue(BitstreamUtils.hasAv1SequenceHeader(temporalUnit))
        assertFalse(BitstreamUtils.hasAv1SequenceHeader(frameObu))

        // av1C CSD-0 box: 0x81 + 3 config bytes + seqObu
        val av1c = byteArrayOf(0x81.toByte(), 0x04, 0x0C, 0x00) + seqObu
        val extracted = BitstreamUtils.extractAv1SequenceHeaderObu(av1c)
        assertNotNull(extracted)
        assertArrayEquals(seqObu, extracted)

        // Accumulator prepends seq header onto keyframe missing it
        val acc = BitstreamUtils.CodecConfigAccumulator(StreamCodec.AV1)
        assertArrayEquals(seqObu, acc.ingest(av1c))
        val prepared = acc.ensureConfigOnKeyFrame(frameObu)
        assertArrayEquals(seqObu + frameObu, prepared)
    }

    @Test
    fun ivfHeadersFollowStandardLittleEndianLayout() {
        val fileHdr = BitstreamUtils.buildIvfFileHeader(1920, 1080, 1_000_000)
        assertEquals(32, fileHdr.size)
        assertEquals("DKIF", String(fileHdr, 0, 4))
        assertEquals("AV01", String(fileHdr, 8, 4))
        val bb = ByteBuffer.wrap(fileHdr).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(0.toShort(), bb.getShort(4))
        assertEquals(32.toShort(), bb.getShort(6))
        assertEquals(1920.toShort(), bb.getShort(12))
        assertEquals(1080.toShort(), bb.getShort(14))
        assertEquals(1_000_000, bb.getInt(16))
        assertEquals(1, bb.getInt(20))

        val frameHdr = BitstreamUtils.buildIvfFrameHeader(4096, 66_666L)
        assertEquals(12, frameHdr.size)
        val fb = ByteBuffer.wrap(frameHdr).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(4096, fb.getInt(0))
        assertEquals(66_666L, fb.getLong(4))
    }
}
