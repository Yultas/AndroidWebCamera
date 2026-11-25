package com.androidwebcamera.camera

import android.content.Context
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.StreamConfigurationMap
import android.media.MediaCodec
import android.util.Log
import android.util.Range
import android.util.Size
import com.androidwebcamera.model.CameraDescriptor
import com.androidwebcamera.model.LensFacing
import com.androidwebcamera.model.VideoResolution
import java.util.Locale
import kotlin.math.abs

/**
 * Discovers Camera2 devices (including physical sub-lenses inside logical multi-cameras) and
 * extracts hardware-validated resolutions and FPS modes across regular, constrained high-speed,
 * and OEM vendor video configurations while preserving zero-copy surface encoding.
 */
object CameraEnumerator {

    private const val TAG = "CameraEnumerator"

    /**
     * Identifies either a standalone Camera2 device (`"0"`) or a physical sub-camera routed
     * through a logical multi-camera (`"0:52"` -> open `"0"`, bind `OutputConfiguration` to `"52"`).
     */
    data class CameraTargetId(
        val logicalId: String,
        val physicalId: String? = null
    ) {
        val effectiveCharacteristicsId: String
            get() = physicalId ?: logicalId
    }

    fun parseCameraId(rawId: String): CameraTargetId {
        val idx = rawId.indexOf(':')
        if (idx <= 0 || idx >= rawId.lastIndex) {
            return CameraTargetId(logicalId = rawId, physicalId = null)
        }
        return CameraTargetId(
            logicalId = rawId.substring(0, idx),
            physicalId = rawId.substring(idx + 1)
        )
    }

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
        val isPhysicalSubCamera: Boolean,
        val resolutions: List<VideoResolution>,
        val fpsByResolution: Map<VideoResolution, List<Int>>,
        val highSpeedFpsByResolution: Map<VideoResolution, List<Int>>
    )

    fun enumerate(context: Context): List<CameraDescriptor> {
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as? CameraManager
            ?: return emptyList()

        val topLevelIds = runCatching { manager.cameraIdList.toList() }.getOrDefault(emptyList())
        val topLevelSet = topLevelIds.toSet()
        val rawList = mutableListOf<RawCameraInfo>()
        val topLevelMap = mutableMapOf<String, RawCameraInfo>()

        for (cameraId in topLevelIds) {
            val info = runCatching {
                inspectCamera(
                    manager = manager,
                    descriptorId = cameraId,
                    characteristicsId = cameraId,
                    isPhysicalSubCamera = false
                )
            }.getOrNull()
            if (info != null && info.resolutions.isNotEmpty()) {
                rawList.add(info)
                topLevelMap[cameraId] = info
            }
        }

        // Discover hidden physical sub-lenses (ultrawide, telephoto, macro) inside LOGICAL_MULTI_CAMERA
        for (logicalId in topLevelIds) {
            val logicalChars = runCatching { manager.getCameraCharacteristics(logicalId) }.getOrNull()
                ?: continue
            val caps = logicalChars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
            val isLogicalMulti = caps != null && caps.contains(
                CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA
            )
            if (!isLogicalMulti) continue

            val parentInfo = topLevelMap[logicalId]
            val physicalIds = runCatching { logicalChars.physicalCameraIds }.getOrDefault(emptySet())
            for (physId in physicalIds) {
                if (physId in topLevelSet) continue
                val compositeId = "$logicalId:$physId"
                val physInfo = runCatching {
                    inspectCamera(
                        manager = manager,
                        descriptorId = compositeId,
                        characteristicsId = physId,
                        isPhysicalSubCamera = true
                    )
                }.getOrNull() ?: continue
                if (physInfo.resolutions.isEmpty()) continue

                // Skip the primary 1x sensor that already backs the parent logical camera
                if (parentInfo != null &&
                    physInfo.facing == parentInfo.facing &&
                    physInfo.focalLength != null &&
                    parentInfo.focalLength != null &&
                    abs(physInfo.focalLength - parentInfo.focalLength) < 0.08f
                ) {
                    continue
                }

                // Skip physical sub-camera if an identical facing+focal lens was already discovered
                val duplicateLens = rawList.any { existing ->
                    existing.facing == physInfo.facing &&
                        existing.focalLength != null &&
                        physInfo.focalLength != null &&
                        abs(existing.focalLength - physInfo.focalLength) < 0.05f
                }
                if (!duplicateLens) {
                    rawList.add(physInfo)
                }
            }
        }

        val backFocalSorted = rawList
            .filter { it.facing == LensFacing.BACK && it.focalLength != null }
            .sortedBy { it.focalLength }

        val mainBackFocal = rawList.firstOrNull { it.id == "0" && it.facing == LensFacing.BACK }?.focalLength
            ?: backFocalSorted.getOrNull(backFocalSorted.size / 2)?.focalLength

        val frontIndexMap = rawList
            .filter { it.facing == LensFacing.FRONT }
            .mapIndexed { idx, info -> info.id to idx }
            .toMap()

        return rawList.map { raw ->
            val friendlyName = buildCameraName(raw, backFocalSorted, mainBackFocal, frontIndexMap)
            CameraDescriptor(
                id = raw.id,
                name = friendlyName,
                facing = raw.facing,
                focalLengthMm = raw.focalLength,
                sensorOrientation = raw.sensorOrientation,
                hasFlash = raw.hasFlash,
                supportedResolutions = raw.resolutions,
                supportedFpsByResolution = raw.fpsByResolution,
                highSpeedFpsByResolution = raw.highSpeedFpsByResolution
            )
        }
    }

    /**
     * Returns true when [targetFps] at [width]x[height] is only achievable via
     * [android.hardware.camera2.params.SessionConfiguration.SESSION_HIGH_SPEED].
     */
    fun shouldUseHighSpeedSession(
        characteristics: CameraCharacteristics,
        width: Int,
        height: Int,
        targetFps: Int,
        isPhysicalSubCamera: Boolean = false
    ): Boolean {
        if (isPhysicalSubCamera || targetFps <= 30) return false
        val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return false
        val size = Size(width, height)
        val minDurationNs = runCatching {
            map.getOutputMinFrameDuration(MediaCodec::class.java, size)
        }.getOrDefault(0L)
        val maxSensorFps = if (minDurationNs > 0L) {
            (1_000_000_000.0 / minDurationNs.toDouble() + 0.5).toInt()
        } else {
            0
        }
        val vendorMaxFps = extractSamsungVideoMaxFps(characteristics)[VideoResolution(width, height)] ?: 0
        if (targetFps <= maxOf(maxSensorFps, vendorMaxFps)) {
            return false
        }

        val caps = characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
        val hasHighSpeed = caps != null && caps.contains(
            CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO
        )
        if (!hasHighSpeed) return false
        val hsSizes = runCatching { map.highSpeedVideoSizes?.toSet() }.getOrNull().orEmpty()
        if (size !in hsSizes) return false
        val hsRanges = runCatching { map.getHighSpeedVideoFpsRangesFor(size)?.toList() }.getOrNull().orEmpty()
        return hsRanges.any { it.upper == targetFps }
    }

    /**
     * Picks the best AE target FPS range for regular, constrained high-speed, or extended OEM high-FPS sessions.
     */
    fun selectFpsRange(
        characteristics: CameraCharacteristics,
        width: Int,
        height: Int,
        targetFps: Int,
        useHighSpeed: Boolean = false
    ): Range<Int>? {
        if (useHighSpeed) {
            val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            val hsRanges = runCatching {
                map?.getHighSpeedVideoFpsRangesFor(Size(width, height))?.toList()
            }.getOrNull().orEmpty()
            hsRanges.firstOrNull { it.lower == targetFps && it.upper == targetFps }?.let { return it }
            hsRanges.filter { it.upper == targetFps }
                .maxByOrNull { it.lower }
                ?.let { return it }
        }

        val ranges = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.toList()
            .orEmpty()
        if (ranges.isEmpty()) return Range(targetFps, targetFps)

        val maxAeUpper = ranges.maxOfOrNull { it.upper } ?: 30
        if (targetFps > maxAeUpper) {
            // OEM HALs (e.g. Samsung Exynos/OneUI) omit 60fps from CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES
            // even when 60fps is supported via minFrameDuration or vendor video configurations.
            return Range(targetFps, targetFps)
        }

        val hwLevel = characteristics.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)
        val preferVariable =
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

        ranges.filter { targetFps in it.lower..it.upper }
            .minByOrNull { (it.upper - targetFps) * 100 + (it.upper - it.lower) }
            ?.let { return it }

        return ranges.minByOrNull { abs(it.upper - targetFps) }
    }

    fun selectAfMode(characteristics: CameraCharacteristics): Int {
        val modes = characteristics.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES)
            ?.toList()
            .orEmpty()
        return when {
            CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO in modes ->
                CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO
            CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE in modes ->
                CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE
            CaptureRequest.CONTROL_AF_MODE_AUTO in modes ->
                CaptureRequest.CONTROL_AF_MODE_AUTO
            else -> CaptureRequest.CONTROL_AF_MODE_OFF
        }
    }

    /**
     * Applies optional OEM vendor session/request keys (such as Samsung OneUI's `recordingMaxFps`)
     * when driving sensor frame rates above standard `CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES`.
     * All lookups are guarded with [runCatching] so non-Samsung devices safely no-op.
     */
    fun applyVendorHighFpsKeys(builder: CaptureRequest.Builder, targetFps: Int) {
        setVendorKeySafely(builder, "samsung.android.control.recordingMaxFps", Int::class.javaPrimitiveType!!, targetFps)
        setVendorKeySafely(builder, "samsung.android.sensor.streamType", Int::class.javaPrimitiveType!!, 0)
        setVendorKeySafely(builder, "samsung.android.control.recordingExtraMode", Int::class.javaPrimitiveType!!, 0)
        setVendorKeySafely(builder, "samsung.android.control.swVideoStabilization", Int::class.javaPrimitiveType!!, 0)
        setVendorKeySafely(builder, "samsung.android.control.videoBeautyFace", Boolean::class.javaPrimitiveType!!, false)
    }

    private fun <T : Any> setVendorKeySafely(
        builder: CaptureRequest.Builder,
        keyName: String,
        type: Class<T>,
        value: T
    ) {
        runCatching {
            val key = CaptureRequest.Key(keyName, type)
            builder.set(key, value)
        }
    }

    private fun inspectCamera(
        manager: CameraManager,
        descriptorId: String,
        characteristicsId: String,
        isPhysicalSubCamera: Boolean
    ): RawCameraInfo? {
        val chars = manager.getCameraCharacteristics(characteristicsId)
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

        val sensorHasHighSpeedPll = capabilities != null &&
            capabilities.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO)
        val hasConstrainedHighSpeed = !isPhysicalSubCamera && sensorHasHighSpeedPll

        val codecSizes = map.getOutputSizes(MediaCodec::class.java)?.toList().orEmpty()
        val yuvSizes = map.getOutputSizes(ImageFormat.YUV_420_888)?.toList().orEmpty()
        val highSpeedSizes = if (hasConstrainedHighSpeed) {
            runCatching { map.highSpeedVideoSizes?.toList() }.getOrNull().orEmpty()
        } else {
            emptyList()
        }
        val vendorVideoMaxFps = extractSamsungVideoMaxFps(chars)

        // Do NOT intersect with SurfaceTexture sizes: LIMITED HALs often cap SurfaceTexture at 1080p
        // even when MediaCodec + YUV_420_888 genuinely support 1440p and 4K.
        val codecResSet = codecSizes.map { VideoResolution(it.width, it.height) }.toSet()
        val yuvResSet = yuvSizes.map { VideoResolution(it.width, it.height) }.toSet()
        val baseSizes = if (codecResSet.isNotEmpty() && yuvResSet.isNotEmpty()) {
            (codecResSet intersect yuvResSet).ifEmpty { codecResSet }
        } else {
            codecResSet.ifEmpty { yuvResSet }
        }

        val extraSizes = (highSpeedSizes.map { VideoResolution(it.width, it.height) } + vendorVideoMaxFps.keys)
            .filter { codecResSet.isEmpty() || it in codecResSet }

        val validSizes = (baseSizes + extraSizes)
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
                validSizes.sortedWith(
                    compareByDescending<VideoResolution> { it.width * it.height }
                        .thenByDescending { it.width }
                )
            }

        val aeRanges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.toList()
            .orEmpty()
        val maxAeUpper = aeRanges.maxOfOrNull { it.upper } ?: 30
        val highSpeedSizeSet = highSpeedSizes.toSet()

        val fpsByResolution = mutableMapOf<VideoResolution, List<Int>>()
        val highSpeedByResolution = mutableMapOf<VideoResolution, List<Int>>()

        for (res in selectedResolutions) {
            val (supported, highSpeedOnly) = computeSupportedFps(
                map = map,
                resolution = res,
                maxAeUpper = maxAeUpper,
                aeRanges = aeRanges,
                vendorMaxFps = vendorVideoMaxFps[res] ?: 0,
                sensorHasHighSpeedPll = sensorHasHighSpeedPll,
                hasConstrainedHighSpeed = hasConstrainedHighSpeed,
                highSpeedSizeSet = highSpeedSizeSet
            )
            fpsByResolution[res] = supported
            if (highSpeedOnly.isNotEmpty()) {
                highSpeedByResolution[res] = highSpeedOnly
            }
        }

        Log.i(
            TAG,
            "Camera id=$descriptorId facing=$facing focal=$focal " +
                "modes=${selectedResolutions.joinToString { "${it.width}x${it.height}@${fpsByResolution[it]}" }}"
        )

        return RawCameraInfo(
            id = descriptorId,
            facing = facing,
            focalLength = focal,
            sensorOrientation = sensorOrientation,
            hasFlash = hasFlash,
            isPhysicalSubCamera = isPhysicalSubCamera,
            resolutions = selectedResolutions,
            fpsByResolution = fpsByResolution,
            highSpeedFpsByResolution = highSpeedByResolution
        )
    }

    /**
     * Reads Samsung's `samsung.android.scaler.availableVideoConfigurations` (`int32[N*6]`:
     * `[width, height, minFps, maxFps, ...]`) when present on OneUI devices.
     */
    private fun extractSamsungVideoMaxFps(chars: CameraCharacteristics): Map<VideoResolution, Int> {
        val raw = runCatching {
            val key = CameraCharacteristics.Key(
                "samsung.android.scaler.availableVideoConfigurations",
                IntArray::class.java
            )
            chars.get(key)
        }.getOrNull() ?: return emptyMap()

        if (raw.size < 6) return emptyMap()
        val result = mutableMapOf<VideoResolution, Int>()
        var i = 0
        while (i + 5 < raw.size) {
            val w = raw[i]
            val h = raw[i + 1]
            val fpsMax = raw[i + 3]
            if (w in 320..3840 && h in 240..2160 && fpsMax in 15..240) {
                val res = VideoResolution(w, h)
                val prev = result[res] ?: 0
                if (fpsMax > prev) {
                    result[res] = fpsMax
                }
            }
            i += 6
        }
        return result
    }

    private fun computeSupportedFps(
        map: StreamConfigurationMap,
        resolution: VideoResolution,
        maxAeUpper: Int,
        aeRanges: List<Range<Int>>,
        vendorMaxFps: Int,
        sensorHasHighSpeedPll: Boolean,
        hasConstrainedHighSpeed: Boolean,
        highSpeedSizeSet: Set<Size>
    ): Pair<List<Int>, List<Int>> {
        val size = Size(resolution.width, resolution.height)
        val minDurationNs = runCatching {
            map.getOutputMinFrameDuration(MediaCodec::class.java, size)
        }.getOrDefault(0L)
        val maxSensorFps = if (minDurationNs > 0L) {
            (1_000_000_000.0 / minDurationNs.toDouble() + 0.5).toInt()
        } else {
            maxAeUpper
        }

        val highSpeedExactUppers = if (hasConstrainedHighSpeed && size in highSpeedSizeSet) {
            runCatching { map.getHighSpeedVideoFpsRangesFor(size)?.map { it.upper }?.toSet() }
                .getOrNull()
                .orEmpty()
        } else {
            emptySet()
        }

        val maxRegularOrVendorFps = maxOf(maxSensorFps, vendorMaxFps)
        val supported = mutableListOf<Int>()
        val highSpeedOnly = mutableListOf<Int>()

        for (target in candidateFps) {
            val regularAeSupported = target <= maxSensorFps &&
                (aeRanges.isEmpty() || aeRanges.any { target in it.lower..it.upper || it.upper >= target })
            // When AE target FPS ranges stop at 30 (e.g. Samsung OneUI), auxiliary fixed-30fps lenses
            // sometimes copy 60fps metadata into minFrameDuration/vendor configs without V4L2 60fps PLL support.
            // Require sensorHasHighSpeedPll before unlocking >maxAeUpper via extended regular/vendor metadata.
            val extendedRegularSupported = target <= maxRegularOrVendorFps &&
                (target <= maxAeUpper || sensorHasHighSpeedPll)
            val highSpeedSupported = target in highSpeedExactUppers

            if (!regularAeSupported && !extendedRegularSupported && !highSpeedSupported) {
                continue
            }
            if (target > 30 && !CodecCapabilityDetector.isHardwareEncodeSupported(resolution.width, resolution.height, target)) {
                continue
            }
            supported.add(target)
            if (!regularAeSupported && !extendedRegularSupported && highSpeedSupported) {
                highSpeedOnly.add(target)
            }
        }

        val finalSupported = supported.ifEmpty {
            listOf(minOf(30, maxRegularOrVendorFps).coerceAtLeast(15))
        }
        return finalSupported to highSpeedOnly
    }

    private fun buildCameraName(
        raw: RawCameraInfo,
        backFocalSorted: List<RawCameraInfo>,
        mainBackFocal: Float?,
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
                val focal = raw.focalLength
                val refMain = mainBackFocal ?: 4.3f
                val baseLabel = when {
                    backFocalSorted.size <= 1 -> "Основная"
                    raw.id == "0" -> "Основная"
                    focal != null && focal < minOf(3.2f, refMain * 0.78f) -> "Широкоугольная"
                    focal != null && focal > maxOf(5.3f, refMain * 1.22f) -> "Телефото"
                    else -> "Тыловая #${raw.id}"
                }
                "$baseLabel$focalSuffix"
            }
        }
    }
}
