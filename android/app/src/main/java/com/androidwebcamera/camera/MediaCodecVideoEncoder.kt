package com.androidwebcamera.camera

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Bundle
import android.util.Log
import android.view.Surface
import com.androidwebcamera.model.StreamCodec
import com.androidwebcamera.server.EncodedVideoFrame
import com.androidwebcamera.server.FrameHub
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Hardware-preferred MediaCodec video encoder supporting H.264 (AVC), H.265 (HEVC), and AV1.
 * Accepts frames zero-copy on an input [Surface] and publishes packetized frames into [FrameHub].
 */
class MediaCodecVideoEncoder(
    private val codecType: StreamCodec,
    private val width: Int,
    private val height: Int,
    private val fps: Int,
    private val bitrateKbps: Int,
    private val generation: Long,
    private val frameHub: FrameHub,
    private val onFatalError: (Throwable) -> Unit
) : VideoEncoder {

    private val mimeType: String = requireNotNull(codecType.mimeType) {
        "Codec ${codecType.apiValue} is not backed by MediaCodec"
    }

    private val running = AtomicBoolean(false)
    private val codec: MediaCodec
    override val inputSurface: Surface
    private var drainThread: Thread? = null

    private val configAccumulator = BitstreamUtils.CodecConfigAccumulator(codecType)

    @Volatile
    private var lastPublishedConfig: ByteArray? = null

    @Volatile
    private var firstFrameLogged = false

    override val hasProducedFrame: Boolean
        get() = firstFrameLogged

    init {
        val (configuredCodec, surface) = initializeCodecWithFallback()
        codec = configuredCodec
        inputSurface = surface
        Log.i(
            TAG,
            "Initialized ${codecType.apiValue.uppercase()} encoder=${codec.name} " +
                "${width}x${height}@${fps}fps ${bitrateKbps}kbps gen=$generation"
        )
    }

    override fun start() {
        if (!running.compareAndSet(false, true)) return
        firstFrameLogged = false
        codec.start()
        drainThread = Thread(::drainLoop, "Awc${codecType.apiValue.uppercase()}Drain").apply {
            isDaemon = true
            start()
        }
    }

    override fun requestSyncFrame() {
        if (!running.get()) return
        runCatching {
            val params = Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            }
            codec.setParameters(params)
        }
    }

    override fun updateBitrate(bitrateKbps: Int) {
        if (!running.get()) return
        runCatching {
            val params = Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrateKbps * 1000)
            }
            codec.setParameters(params)
            Log.i(TAG, "Updated ${codecType.apiValue.uppercase()} bitrate to ${bitrateKbps}kbps")
        }
    }

    override fun stopAndRelease() {
        running.set(false)
        runCatching { codec.signalEndOfInputStream() }
        val thread = drainThread
        drainThread = null
        if (thread != null && thread !== Thread.currentThread()) {
            runCatching { thread.join(900) }
            if (thread.isAlive) {
                thread.interrupt()
                runCatching { thread.join(200) }
            }
        }
        runCatching { codec.stop() }
        runCatching { codec.release() }
        runCatching { inputSurface.release() }
    }

    private fun initializeCodecWithFallback(): Pair<MediaCodec, Surface> {
        val candidates = CodecCapabilityDetector.findEncodersFor(codecType, width, height, fps)
        var lastError: Throwable? = null

        val candidateNames: List<String?> = if (candidates.isNotEmpty()) {
            candidates.map { it.name }
        } else {
            listOf(null) // fallback to createEncoderByType
        }

        for (candidateName in candidateNames) {
            var created: MediaCodec? = null
            try {
                created = if (candidateName != null) {
                    MediaCodec.createByCodecName(candidateName)
                } else {
                    MediaCodec.createEncoderByType(mimeType)
                }
                configureSingleCodec(created)
                val surface = created.createInputSurface()
                return created to surface
            } catch (t: Throwable) {
                lastError = t
                Log.w(TAG, "Encoder candidate ${candidateName ?: mimeType} failed: ${t.message}")
                runCatching { created?.release() }
            }
        }

        throw IllegalStateException(
            "Кодек ${codecType.displayName} не поддерживается на этом устройстве" +
                (lastError?.message?.let { ": $it" } ?: ""),
            lastError
        )
    }

    private fun configureSingleCodec(target: MediaCodec) {
        val codecName = runCatching { target.name }.getOrDefault("")
        val caps = runCatching {
            target.codecInfo.getCapabilitiesForType(mimeType)
        }.getOrNull()
        val isHisi = codecName.contains("hisi", ignoreCase = true)
        val supportsLowLatency = caps?.isFeatureSupported(
            MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency
        ) == true && !isHisi

        val preferredBitrateMode = if (codecType == StreamCodec.H265 &&
            caps?.encoderCapabilities?.isBitrateModeSupported(
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR
            ) == true
        ) {
            MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR
        } else if (caps?.encoderCapabilities?.isBitrateModeSupported(
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR
            ) == true
        ) {
            MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR
        } else {
            null
        }

        val preferredFormat = buildBaseFormat().apply {
            if (preferredBitrateMode != null) {
                setInteger(MediaFormat.KEY_BITRATE_MODE, preferredBitrateMode)
            }
            // Disable B-frames so hardware HEVC/AVC encoders emit strictly monotonic, zero-reorder frames.
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            // Lock hardware encoder clock to realtime pacing to eliminate Exynos/Qualcomm batch bursts.
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            setFloat(MediaFormat.KEY_OPERATING_RATE, fps.toFloat())
            if (supportsLowLatency) {
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                setInteger(MediaFormat.KEY_LATENCY, 0)
            }
            if ((codecType == StreamCodec.H264 || codecType == StreamCodec.H265) && !isHisi) {
                setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
            }
        }

        try {
            target.configure(preferredFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        } catch (e: Exception) {
            Log.w(TAG, "Preferred MediaFormat rejected by $codecName, falling back to baseline", e)
            target.reset()
            target.configure(buildBaseFormat(), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }
    }

    private fun buildBaseFormat(): MediaFormat =
        MediaFormat.createVideoFormat(mimeType, width, height).apply {
            setInteger(
                MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface
            )
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateKbps * 1000)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
        }

    private fun drainLoop() {
        val info = MediaCodec.BufferInfo()
        try {
            while (running.get() && !Thread.currentThread().isInterrupted) {
                val index = codec.dequeueOutputBuffer(info, 25_000L)
                when {
                    index == MediaCodec.INFO_TRY_AGAIN_LATER -> Unit
                    index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        extractCsdFromFormat(codec.outputFormat)?.let { rawCsd ->
                            val combined = configAccumulator.ingest(rawCsd) ?: rawCsd
                            publishConfigIfChanged(combined, 0L)
                        }
                    }

                    index >= 0 -> {
                        try {
                            val outBuf = codec.getOutputBuffer(index)
                            if (outBuf != null && info.size > 0) {
                                handleOutputBuffer(outBuf, info)
                            }
                        } finally {
                            codec.releaseOutputBuffer(index, false)
                        }
                        if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                            break
                        }
                    }
                }
            }
        } catch (t: Throwable) {
            if (running.get()) {
                Log.e(TAG, "${codecType.apiValue.uppercase()} drain loop error", t)
                onFatalError(t)
            }
        }
    }

    private fun handleOutputBuffer(buffer: ByteBuffer, info: MediaCodec.BufferInfo) {
        val payload = ByteArray(info.size)
        buffer.position(info.offset)
        buffer.limit(info.offset + info.size)
        buffer.get(payload)

        val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
        val flagKeyFrame = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0

        if (isConfig) {
            val combined = configAccumulator.ingest(payload)
            if (combined != null) {
                publishConfigIfChanged(combined, info.presentationTimeUs)
            }
            return
        }

        val isKeyFrame = flagKeyFrame || when (codecType) {
            StreamCodec.H264 -> BitstreamUtils.isH264KeyFrame(payload)
            StreamCodec.H265 -> BitstreamUtils.isHevcKeyFrame(payload)
            StreamCodec.AV1 -> BitstreamUtils.hasAv1SequenceHeader(payload)
            StreamCodec.MJPEG -> true
        }

        val finalPayload = if (isKeyFrame) {
            val prevCfg = configAccumulator.currentConfigBytes()
            val prepared = configAccumulator.ensureConfigOnKeyFrame(payload)
            val newCfg = configAccumulator.currentConfigBytes()
            if (newCfg != null && (prevCfg == null || !newCfg.contentEquals(lastPublishedConfig))) {
                publishConfigIfChanged(newCfg, info.presentationTimeUs)
            }
            prepared
        } else {
            payload
        }

        if (!firstFrameLogged) {
            firstFrameLogged = true
            Log.i(
                TAG,
                "First encoded ${codecType.apiValue.uppercase()} frame published " +
                    "(${finalPayload.size} bytes, key=$isKeyFrame, gen=$generation)"
            )
        }

        frameHub.publishVideo(
            EncodedVideoFrame(
                codec = codecType,
                data = finalPayload,
                ptsUs = info.presentationTimeUs,
                isConfig = false,
                isKeyFrame = isKeyFrame,
                generation = generation
            )
        )
    }

    private fun publishConfigIfChanged(configBytes: ByteArray, ptsUs: Long) {
        if (lastPublishedConfig?.contentEquals(configBytes) == true) return
        lastPublishedConfig = configBytes.copyOf()
        frameHub.publishVideo(
            EncodedVideoFrame(
                codec = codecType,
                data = configBytes,
                ptsUs = ptsUs,
                isConfig = true,
                isKeyFrame = false,
                generation = generation
            )
        )
    }

    private fun extractCsdFromFormat(format: MediaFormat): ByteArray? {
        val csd0 = format.getByteBuffer("csd-0")?.toByteArray() ?: return null
        val csd1 = format.getByteBuffer("csd-1")?.toByteArray()
        val csd2 = format.getByteBuffer("csd-2")?.toByteArray()
        var combined = csd0
        if (csd1 != null) combined += csd1
        if (csd2 != null) combined += csd2
        return combined
    }

    private fun ByteBuffer.toByteArray(): ByteArray {
        val copy = duplicate()
        copy.position(0)
        val arr = ByteArray(copy.remaining())
        copy.get(arr)
        return arr
    }

    companion object {
        private const val TAG = "MediaCodecVideoEncoder"
    }
}
