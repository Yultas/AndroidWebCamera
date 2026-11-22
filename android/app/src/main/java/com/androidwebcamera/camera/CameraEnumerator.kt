package com.androidwebcamera.camera

import android.content.Context
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.params.StreamConfigurationMap
import android.media.MediaCodec
import android.util.Range
import android.util.Size
import com.androidwebcamera.model.CameraDescriptor
import com.androidwebcamera.model.LensFacing
import com.androidwebcamera.model.VideoResolution
import java.util.Locale
import kotlin.math.abs

/**
 * Discovers Camera2 devices and extracts hardware-validated resolutions and FPS ranges
 * compatible with MediaCodec surface encoding and ImageReader YUV capture.
 */
object CameraEnumerator {

    private val preferredResolutions = listOf(
        VideoResolution(3840, 2160),
        VideoResolution(2560, 1440),
        VideoResolution(1920, 1080),
        VideoResolution(1600, 900),
        VideoResolution(1280, 720),
        VideoResolution(960, 540),
        VideoResolution(848, 480),
        VideoResolution(640, 480),
        VideoResolution(640, 360),
        VideoResolution(480, 360)
    )

    private val candidateFps = listOf(15, 24, 30, 60)

    private data class RawCameraInfo(
        val id: String,
        val facing: LensFacing,
        val focalLength: Float?,
        val sensorOrientation: Int,
        val hasFlash: Boolean,
        val resolutions: List<VideoResolution>,
        val fpsByResolution: Map<VideoResolution, List<Int>>
    )

    fun enumerate(context: Context): List<CameraDescriptor> {
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as? CameraManager
            ?: return emptyList()

        val rawList = mutableListOf<RawCameraInfo>()
        for (cameraId in manager.cameraIdList) {
            val info = runCatching { inspectCamera(manager, cameraId) }.getOrNull()
            if (info != null && info.resolutions.isNotEmpty()) {
                rawList.add(info)
            }
        }

        val backFocalSorted = rawList
            .filter { it.facing == LensFacing.BACK && it.focalLength != null }
            .sortedBy { it.focalLength }

        val frontIndexMap = rawList
            .filter { it.facing == LensFacing.FRONT }
            .mapIndexed { idx, info -> info.id to idx }
            .toMap()

        return rawList.map { raw ->
            val friendlyName = buildCameraName(raw, backFocalSorted, frontIndexMap)
            CameraDescriptor(
                id = raw.id,
                name = friendlyName,
                facing = raw.facing,
                focalLengthMm = raw.focalLength,
                sensorOrientation = raw.sensorOrientation,
                hasFlash = raw.hasFlash,
                supportedResolutions = raw.resolutions,
                supportedFpsByResolution = raw.fpsByResolution
            )
        }
    }

    /**
     * Picks the safest AE target FPS range for the requested frame rate.
     * On LIMITED/LEGACY devices or >= 60fps targets, prefers variable ranges ending at targetFps
     * (e.g. [30, 60] or [15, 30]) to prevent HAL frame-duration watchdogs in low light.
     */
    fun selectFpsRange(characteristics: CameraCharacteristics, targetFps: Int): Range<Int>? {
        val ranges = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.toList()
            .orEmpty()
        if (ranges.isEmpty()) return null

        val hwLevel = characteristics.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)
        val preferVariable = targetFps >= 60 ||
            hwLevel == CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY ||
            hwLevel == CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED

        if (preferVariable) {
            ranges.filter { it.upper == targetFps && it.lower < targetFps }
                .maxByOrNull { it.lower }
                ?.let { return it }
            ranges.firstOrNull { it.lower == targetFps && it.upper == targetFps }?.let { return it }
        } else {
            ranges.firstOrNull { it.lower == targetFps && it.upper == targetFps }?.let { return it }
            ranges.filter { it.upper == targetFps && it.lower <= targetFps }
                .maxByOrNull { it.lower }
                ?.let { return it }
        }

        // Any range covering targetFps, preferring lowest upper >= targetFps
        ranges.filter { targetFps in it.lower..it.upper }
            .minByOrNull { (it.upper - targetFps) * 100 + (it.upper - it.lower) }
            ?.let { return it }

        // Fallback to closest upper bound
        return ranges.minByOrNull { abs(it.upper - targetFps) }
    }

    private fun inspectCamera(manager: CameraManager, cameraId: String): RawCameraInfo? {
        val chars = manager.getCameraCharacteristics(cameraId)
        val capabilities = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
        if (capabilities != null &&
            !capabilities.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE)
        ) {
            return null
        }
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return null

        val facing = when (chars.get(CameraCharacteristics.LENS_FACING)) {
            CameraCharacteristics.LENS_FACING_FRONT -> LensFacing.FRONT
            CameraCharacteristics.LENS_FACING_EXTERNAL -> LensFacing.EXTERNAL
            else -> LensFacing.BACK
        }

        val focalLengths = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
        val focal = focalLengths?.firstOrNull()
        val sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
        val hasFlash = chars.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true

        val codecSizes = map.getOutputSizes(MediaCodec::class.java)?.toList().orEmpty()
        val yuvSizes = map.getOutputSizes(ImageFormat.YUV_420_888)?.toList().orEmpty()
        val previewSizes = map.getOutputSizes(SurfaceTexture::class.java)?.toList().orEmpty()

        val commonSizes = if (codecSizes.isNotEmpty() && yuvSizes.isNotEmpty()) {
            val yuvSet = yuvSizes.map { VideoResolution(it.width, it.height) }.toSet()
            val previewSet = previewSizes.map { VideoResolution(it.width, it.height) }.toSet()
            val allThree = codecSizes
                .map { VideoResolution(it.width, it.height) }
                .filter { it in yuvSet && (previewSet.isEmpty() || it in previewSet) }
            allThree.ifEmpty {
                codecSizes.map { VideoResolution(it.width, it.height) }.filter { it in yuvSet }
            }.ifEmpty {
                codecSizes.map { VideoResolution(it.width, it.height) }
            }
        } else {
            (codecSizes.ifEmpty { yuvSizes }).map { VideoResolution(it.width, it.height) }
        }

        val validSizes = commonSizes
            .distinct()
            .filter {
                it.width in 320..3840 &&
                    it.height in 240..2160 &&
                    it.width % 16 == 0 &&
                    it.height % 2 == 0
            }

        val selectedResolutions = preferredResolutions
            .filter { it in validSizes }
            .ifEmpty {
                validSizes.sortedWith(compareByDescending<VideoResolution> { it.width * it.height }
                    .thenByDescending { it.width })
            }

        val aeRanges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.toList()
            .orEmpty()
        val maxAeUpper = aeRanges.maxOfOrNull { it.upper } ?: 30

        val fpsByResolution = selectedResolutions.associateWith { res ->
            computeSupportedFps(map, res, maxAeUpper, aeRanges)
        }

        return RawCameraInfo(
            id = cameraId,
            facing = facing,
            focalLength = focal,
            sensorOrientation = sensorOrientation,
            hasFlash = hasFlash,
            resolutions = selectedResolutions,
            fpsByResolution = fpsByResolution
        )
    }

    private fun computeSupportedFps(
        map: StreamConfigurationMap,
        resolution: VideoResolution,
        maxAeUpper: Int,
        aeRanges: List<Range<Int>>
    ): List<Int> {
        val size = Size(resolution.width, resolution.height)
        val minDurationNs = runCatching {
            map.getOutputMinFrameDuration(MediaCodec::class.java, size)
        }.getOrDefault(0L)
        val maxSensorFps = if (minDurationNs > 0L) {
            (1_000_000_000.0 / minDurationNs.toDouble() + 0.5).toInt()
        } else {
            maxAeUpper
        }
        val effectiveMax = minOf(maxSensorFps, maxAeUpper)

        val valid = candidateFps.filter { target ->
            target <= effectiveMax &&
                (aeRanges.isEmpty() || aeRanges.any { target in it.lower..it.upper || it.upper >= target })
        }
        return valid.ifEmpty { listOf(minOf(30, effectiveMax).coerceAtLeast(15)) }
    }

    private fun buildCameraName(
        raw: RawCameraInfo,
        backFocalSorted: List<RawCameraInfo>,
        frontIndexMap: Map<String, Int>
    ): String {
        val focalSuffix = raw.focalLength?.let { String.format(Locale.US, " (%.1f мм)", it) } ?: ""
        return when (raw.facing) {
            LensFacing.FRONT -> {
                val idx = frontIndexMap[raw.id] ?: 0
                if (idx == 0) "Фронтальная$focalSuffix" else "Фронтальная #${idx + 1}$focalSuffix"
            }

            LensFacing.EXTERNAL -> "Внешняя USB #${raw.id}$focalSuffix"
            LensFacing.BACK -> {
                val baseLabel = when {
                    backFocalSorted.size <= 1 -> "Основная"
                    raw.id == backFocalSorted.first().id && (raw.focalLength ?: 99f) < 3.0f ->
                        "Широкоугольная"

                    raw.id == backFocalSorted.last().id && (raw.focalLength ?: 0f) > 7.0f ->
                        "Телефото"

                    raw.id == "0" -> "Основная"
                    else -> "Тыловая #${raw.id}"
                }
                "$baseLabel$focalSuffix"
            }
        }
    }
}
