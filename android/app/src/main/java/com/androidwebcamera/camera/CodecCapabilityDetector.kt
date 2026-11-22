package com.androidwebcamera.camera

import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.util.Log
import com.androidwebcamera.model.CodecSupportInfo
import com.androidwebcamera.model.StreamCodec

/**
 * Probes Android [MediaCodecList] for available hardware and software video encoders
 * supporting Surface input ([MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface]).
 */
object CodecCapabilityDetector {

    private const val TAG = "CodecCapabilityDetector"

    data class EncoderCandidate(
        val name: String,
        val isHardware: Boolean,
        val supportsRequestedSize: Boolean
    )

    fun detectSupportedCodecs(): List<CodecSupportInfo> {
        val allInfos = runCatching {
            MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.toList()
        }.getOrElse { err ->
            Log.w(TAG, "Failed to query MediaCodecList", err)
            emptyList()
        }

        return StreamCodec.entries.map { codec ->
            if (!codec.isMediaCodec) {
                CodecSupportInfo(
                    codec = codec,
                    supported = true,
                    isHardware = false,
                    encoderName = "YuvImage/JPEG"
                )
            } else {
                val candidates = findCandidates(allInfos, codec.mimeType!!, width = 1280, height = 720)
                val best = candidates.firstOrNull()
                if (best != null) {
                    Log.i(TAG, "Codec ${codec.apiValue}: available via ${best.name} (hw=${best.isHardware})")
                    CodecSupportInfo(
                        codec = codec,
                        supported = true,
                        isHardware = best.isHardware,
                        encoderName = best.name
                    )
                } else {
                    Log.i(TAG, "Codec ${codec.apiValue}: not available on this device")
                    CodecSupportInfo(
                        codec = codec,
                        supported = false,
                        isHardware = false,
                        encoderName = null
                    )
                }
            }
        }
    }

    /**
     * Returns encoder candidates ordered by preference:
     * 1. Hardware encoders supporting the exact requested resolution
     * 2. Hardware encoders (other sizes, e.g. flexible)
     * 3. Software encoders supporting the requested resolution
     * 4. Software encoders (fallback)
     */
    fun findEncodersFor(codec: StreamCodec, width: Int, height: Int): List<EncoderCandidate> {
        val mime = codec.mimeType ?: return emptyList()
        val allInfos = runCatching {
            MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.toList()
        }.getOrDefault(emptyList())
        return findCandidates(allInfos, mime, width, height)
    }

    private fun findCandidates(
        codecInfos: List<MediaCodecInfo>,
        mimeType: String,
        width: Int,
        height: Int
    ): List<EncoderCandidate> {
        val matches = mutableListOf<EncoderCandidate>()
        for (info in codecInfos) {
            if (!info.isEncoder) continue
            if (info.isAlias) continue
            val supportsMime = info.supportedTypes.any { it.equals(mimeType, ignoreCase = true) }
            if (!supportsMime) continue

            val caps = runCatching { info.getCapabilitiesForType(mimeType) }.getOrNull() ?: continue
            val supportsSurface = caps.colorFormats.contains(
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface
            )
            if (!supportsSurface) continue

            val lowerName = info.name.lowercase()
            val isSoftware = info.isSoftwareOnly ||
                lowerName.startsWith("omx.google.") ||
                lowerName.startsWith("c2.android.")
            val isHardware = info.isHardwareAccelerated && !isSoftware

            val sizeSupported = runCatching {
                caps.videoCapabilities?.isSizeSupported(width, height) ?: true
            }.getOrDefault(true)

            matches.add(
                EncoderCandidate(
                    name = info.name,
                    isHardware = isHardware,
                    supportsRequestedSize = sizeSupported
                )
            )
        }

        return matches.sortedWith(
            compareByDescending<EncoderCandidate> { it.isHardware }
                .thenByDescending { it.supportsRequestedSize }
                .thenBy { it.name }
        )
    }
}
