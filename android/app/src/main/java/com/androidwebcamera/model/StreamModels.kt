package com.androidwebcamera.model

/** Supported video streaming codecs. */
enum class StreamCodec(
    val apiValue: String,
    val displayName: String,
    val mimeType: String?,
    val streamPath: String,
    val wsCodecId: Byte
) {
    H264("h264", "H.264", "video/avc", "stream.h264", 1),
    H265("h265", "H.265 (HEVC)", "video/hevc", "stream.h265", 2),
    AV1("av1", "AV1", "video/av01", "stream.av1", 3),
    MJPEG("mjpeg", "MJPEG", null, "stream.mjpg", 0);

    val isMediaCodec: Boolean
        get() = mimeType != null

    companion object {
        fun fromApiOrNull(value: String?): StreamCodec? {
            val normalized = value?.trim()?.lowercase() ?: return null
            return when (normalized) {
                "h264", "avc" -> H264
                "h265", "hevc" -> H265
                "av1", "av01" -> AV1
                "mjpeg", "mjpg" -> MJPEG
                else -> null
            }
        }

        fun fromApi(value: String?): StreamCodec = fromApiOrNull(value) ?: H264
    }
}

/** Device encoder availability for a given [StreamCodec]. */
data class CodecSupportInfo(
    val codec: StreamCodec,
    val supported: Boolean,
    val isHardware: Boolean,
    val encoderName: String? = null
)

/** Physical facing direction of a camera device. */
enum class LensFacing(val label: String) {
    BACK("Тыловая"),
    FRONT("Фронтальная"),
    EXTERNAL("Внешняя")
}

/** Supported video resolution descriptor. */
data class VideoResolution(
    val width: Int,
    val height: Int
) {
    val label: String
        get() = when {
            width == 3840 && height == 2160 -> "3840×2160 (4K)"
            width == 2560 && height == 1440 -> "2560×1440 (1440p)"
            width == 1920 && height == 1080 -> "1920×1080 (1080p)"
            width == 1600 && height == 900 -> "1600×900 (900p)"
            width == 1280 && height == 720 -> "1280×720 (720p)"
            width == 960 && height == 540 -> "960×540 (qHD)"
            width == 848 && height == 480 -> "848×480 (480p)"
            width == 640 && height == 480 -> "640×480 (VGA)"
            width == 640 && height == 360 -> "640×360 (360p)"
            else -> "${width}×${height}"
        }

    val aspectRatio: Float
        get() = if (height > 0) width.toFloat() / height.toFloat() else 16f / 9f
}

/** Capabilities of an enumerated Android camera. */
data class CameraDescriptor(
    val id: String,
    val name: String,
    val facing: LensFacing,
    val focalLengthMm: Float?,
    val sensorOrientation: Int,
    val hasFlash: Boolean,
    val supportedResolutions: List<VideoResolution>,
    val supportedFpsByResolution: Map<VideoResolution, List<Int>>
) {
    fun bestResolutionOrNull(width: Int, height: Int): VideoResolution? =
        supportedResolutions.firstOrNull { it.width == width && it.height == height }
            ?: supportedResolutions.firstOrNull { it.width == 1280 && it.height == 720 }
            ?: supportedResolutions.firstOrNull { it.width == 1920 && it.height == 1080 }
            ?: supportedResolutions.firstOrNull()

    fun supportedFpsFor(resolution: VideoResolution): List<Int> =
        supportedFpsByResolution[resolution] ?: listOf(30)

    /** Picks a preview size with matching aspect ratio capped at 1280x720 for LIMITED HAL stability. */
    fun previewResolutionFor(videoWidth: Int, videoHeight: Int): VideoResolution {
        val targetAspect = if (videoHeight > 0) videoWidth.toFloat() / videoHeight.toFloat() else 16f / 9f
        val capped = supportedResolutions.filter { it.width <= 1280 && it.height <= 720 }
        val sameAspect = capped.filter { kotlin.math.abs(it.aspectRatio - targetAspect) < 0.08f }
        return sameAspect.maxByOrNull { it.width * it.height }
            ?: capped.maxByOrNull { it.width * it.height }
            ?: VideoResolution(minOf(videoWidth, 1280), minOf(videoHeight, 720))
    }
}

/** Active or requested stream parameters. */
data class StreamConfig(
    val cameraId: String = "0",
    val width: Int = 1280,
    val height: Int = 720,
    val fps: Int = 30,
    val codec: StreamCodec = StreamCodec.H264,
    val bitrateKbps: Int = 4000,
    val jpegQuality: Int = 80,
    val port: Int = 8080,
    val rotationDegrees: Int = 0,
    val mirrored: Boolean = false,
    val torchEnabled: Boolean = false
) {
    /** Clamps resolution, FPS, codec availability, and flash flag against device capabilities. */
    fun normalizedFor(
        camera: CameraDescriptor?,
        supportedCodecs: List<CodecSupportInfo> = emptyList()
    ): StreamConfig {
        val normalizedCodec = if (supportedCodecs.isEmpty()) {
            codec
        } else {
            val isSupported = supportedCodecs.any { it.codec == codec && it.supported }
            if (isSupported) {
                codec
            } else {
                supportedCodecs.firstOrNull { it.codec == StreamCodec.H264 && it.supported }?.codec
                    ?: supportedCodecs.firstOrNull { it.supported }?.codec
                    ?: StreamCodec.MJPEG
            }
        }

        val clampedBase = copy(
            codec = normalizedCodec,
            bitrateKbps = bitrateKbps.coerceIn(500, 20000),
            jpegQuality = jpegQuality.coerceIn(40, 95),
            port = port.coerceIn(1024, 65535),
            rotationDegrees = ((rotationDegrees % 360) + 360) % 360
        )

        if (camera == null) return clampedBase
        val resolution = camera.bestResolutionOrNull(width, height)
            ?: VideoResolution(width, height)
        val availableFps = camera.supportedFpsFor(resolution)
        val normalizedFps = when {
            fps in availableFps -> fps
            30 in availableFps -> 30
            else -> availableFps.maxByOrNull { it } ?: fps
        }
        return clampedBase.copy(
            cameraId = camera.id,
            width = resolution.width,
            height = resolution.height,
            fps = normalizedFps,
            torchEnabled = if (camera.hasFlash) torchEnabled else false
        )
    }
}

/** Network interface endpoint where the HTTP/WebSocket server can be reached. */
data class NetworkEndpoint(
    val label: String,
    val interfaceName: String,
    val ip: String,
    val url: String,
    val isUsb: Boolean = false
)

/** State of Android Local-Only Wi-Fi Hotspot. */
sealed interface HotspotState {
    data object Inactive : HotspotState
    data object Starting : HotspotState
    data class Active(val ssid: String, val passphrase: String) : HotspotState
    data class Error(val message: String) : HotspotState
}

/** Complete UI-observable state of the streaming service. */
data class StreamState(
    val isStreaming: Boolean = false,
    val isStarting: Boolean = false,
    val config: StreamConfig = StreamConfig(),
    val availableCameras: List<CameraDescriptor> = emptyList(),
    val activeCamera: CameraDescriptor? = null,
    val supportedCodecs: List<CodecSupportInfo> = defaultCodecSupport(),
    val connectedClients: Int = 0,
    val measuredFps: Int = 0,
    val endpoints: List<NetworkEndpoint> = emptyList(),
    val hotspotState: HotspotState = HotspotState.Inactive,
    val errorMessage: String? = null
) {
    fun isCodecSupported(codec: StreamCodec): Boolean =
        supportedCodecs.firstOrNull { it.codec == codec }?.supported ?: (codec == StreamCodec.MJPEG)

    companion object {
        fun defaultCodecSupport(): List<CodecSupportInfo> = listOf(
            CodecSupportInfo(StreamCodec.H264, supported = true, isHardware = true),
            CodecSupportInfo(StreamCodec.H265, supported = false, isHardware = false),
            CodecSupportInfo(StreamCodec.AV1, supported = false, isHardware = false),
            CodecSupportInfo(StreamCodec.MJPEG, supported = true, isHardware = false)
        )
    }
}
