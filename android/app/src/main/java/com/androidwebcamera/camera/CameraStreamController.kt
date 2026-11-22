package com.androidwebcamera.camera

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.ImageFormat
import android.hardware.HardwareBuffer
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.util.Range
import android.view.Surface
import com.androidwebcamera.model.StreamCodec
import com.androidwebcamera.model.StreamConfig
import com.androidwebcamera.server.FrameHub
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit

/**
 * Owns the Camera2 device, capture session, and video encoder independently of any Activity UI.
 *
 * All state mutations are strictly serialized on a persistent [HandlerThread] (`AwcCamera2`) so
 * closing/reopening devices, hot-reconfiguring streams, and attaching/detaching preview surfaces
 * on screen lock never race or touch an abandoned BufferQueue.
 */
class CameraStreamController(
    context: Context,
    private val frameHub: FrameHub,
    private val onFatalError: (String) -> Unit
) {
    private val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    private val handlerThread = HandlerThread("AwcCamera2").apply { start() }
    private val cameraHandler = Handler(handlerThread.looper)
    private val executor = Executor { command ->
        if (handlerThread.isAlive) {
            cameraHandler.post(command)
        }
    }

    private data class PendingStart(
        val config: StreamConfig,
        val generation: Long
    )

    // All mutable fields below are touched exclusively on cameraHandler.
    private var encoder: VideoEncoder? = null
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var activeFpsRange: Range<Int>? = null
    private var currentConfig: StreamConfig? = null

    /** Desired preview surface requested by UI. */
    private var previewSurface: Surface? = null

    /** Exact preview surface (UI TextureView or background PRIVATE sink) included in [captureSession]. */
    private var sessionPreviewSurface: Surface? = null

    /**
     * Lightweight background ImageReader (`ImageFormat.PRIVATE` + `USAGE_GPU_SAMPLED_IMAGE`) used
     * when the screen is locked or preview is hidden. Many OEM Camera2 HALs (e.g. HiSilicon/Huawei)
     * require a GPU-sampled preview stream alongside MediaCodec at >= 1080p/4K or stall with ERROR_CAMERA_DEVICE (4).
     */
    private var dummyPreviewReader: ImageReader? = null

    /** Callbacks to release detached preview surfaces only after the capture session stops using them. */
    private val deferredPreviewCleanups = mutableListOf<() -> Unit>()

    private var sessionConfiguring = false
    private var needsSessionRebuild = false
    private var closingCamera = false
    private var pendingStart: PendingStart? = null
    private var openingEpoch = 0
    private var released = false
    private var closeWaitLatch: CountDownLatch? = null
    private var sessionRecoveryAttempts = 0
    private var deviceErrorRecoveryAttempts = 0
    private var lastConfiguredElapsedMs = 0L

    private val closeTimeoutRunnable = Runnable {
        if (closingCamera) {
            Log.w(TAG, "Timed out waiting for CameraDevice.onClosed; releasing encoder and proceeding")
            closingCamera = false
            stopEncoderOnlyOnHandler()
            flushDeferredPreviewCleanups()
            closeWaitLatch?.countDown()
            closeWaitLatch = null
            val next = pendingStart
            pendingStart = null
            if (next != null && !released) {
                cameraHandler.postDelayed({
                    if (!released && !closingCamera && cameraDevice == null) {
                        startOnHandler(next.config, next.generation)
                    }
                }, 250L)
            }
        }
    }

    fun start(config: StreamConfig, generation: Long = frameHub.currentGeneration) {
        postOnHandler { startOnHandler(config, generation) }
    }

    fun reconfigure(newConfig: StreamConfig, generation: Long = frameHub.currentGeneration) {
        postOnHandler { reconfigureOnHandler(newConfig, generation) }
    }

    fun setTorch(enabled: Boolean) {
        postOnHandler {
            val cfg = currentConfig ?: return@postOnHandler
            if (cfg.torchEnabled == enabled) return@postOnHandler
            currentConfig = cfg.copy(torchEnabled = enabled)
            applyRepeatingRequestOnHandler()
        }
    }

    fun attachPreviewSurface(surface: Surface) {
        postOnHandler {
            if (released || !surface.isValid || previewSurface === surface) return@postOnHandler
            previewSurface = surface
            if (cameraDevice != null && encoder != null && !closingCamera) {
                scheduleSessionRebuildOnHandler()
            }
        }
    }

    /**
     * Detaches [surface] from the Camera2 pipeline. [onSafeToRelease] is invoked only after
     * the capture session has stopped targeting [surface], preventing HAL BufferQueue abandonment errors.
     */
    fun detachPreviewSurface(surface: Surface, onSafeToRelease: (() -> Unit)? = null) {
        val posted = postOnHandler {
            val wasDesired = previewSurface === surface
            if (wasDesired) {
                previewSurface = null
            }
            val inActiveSession = sessionPreviewSurface === surface
            if (onSafeToRelease != null) {
                if (inActiveSession || (sessionConfiguring && wasDesired)) {
                    deferredPreviewCleanups.add(onSafeToRelease)
                } else {
                    runCatching { onSafeToRelease() }
                }
            }
            if ((wasDesired || inActiveSession) && cameraDevice != null && encoder != null && !closingCamera) {
                scheduleSessionRebuildOnHandler()
            } else if (!inActiveSession && !sessionConfiguring) {
                flushDeferredPreviewCleanups()
            }
        }
        if (!posted && onSafeToRelease != null) {
            runCatching { onSafeToRelease() }
        }
    }

    fun stop() {
        if (Looper.myLooper() == handlerThread.looper) {
            pendingStart = null
            stopInternalOnHandler(closeSubscribers = true)
            return
        }
        val latch = CountDownLatch(1)
        val posted = postOnHandler {
            try {
                pendingStart = null
                stopInternalOnHandler(closeSubscribers = true)
            } finally {
                latch.countDown()
            }
        }
        if (posted) {
            latch.await(1500, TimeUnit.MILLISECONDS)
        }
    }

    fun release() {
        val initiateLatch = CountDownLatch(1)
        var waitForClose: CountDownLatch? = null
        val posted = postOnHandler {
            try {
                released = true
                pendingStart = null
                stopInternalOnHandler(closeSubscribers = true)
                if (closingCamera) {
                    waitForClose = CountDownLatch(1).also { closeWaitLatch = it }
                }
            } finally {
                initiateLatch.countDown()
            }
        }
        if (posted) {
            initiateLatch.await(1500, TimeUnit.MILLISECONDS)
            waitForClose?.await(1500, TimeUnit.MILLISECONDS)
        }
        cameraHandler.removeCallbacksAndMessages(null)
        runCatching { handlerThread.quitSafely() }
    }

    private fun postOnHandler(block: () -> Unit): Boolean {
        if (!handlerThread.isAlive) return false
        return cameraHandler.post(block)
    }

    private fun startOnHandler(config: StreamConfig, generation: Long) {
        if (released) return
        if (closingCamera) {
            pendingStart = PendingStart(config, generation)
            return
        }
        if (cameraDevice != null) {
            pendingStart = PendingStart(config, generation)
            stopInternalOnHandler(closeSubscribers = false)
            return
        }

        stopEncoderOnlyOnHandler()
        currentConfig = config

        val createdEncoder = createEncoder(config, generation) ?: return
        encoder = createdEncoder
        frameHub.onRequestSyncFrame = { createdEncoder.requestSyncFrame() }
        try {
            createdEncoder.start()
        } catch (t: Throwable) {
            stopEncoderOnlyOnHandler()
            val msg = "Не удалось запустить кодировщик ${config.codec.displayName}: ${t.message}"
            Log.e(TAG, msg, t)
            onFatalError(msg)
            return
        }

        openCameraOnHandler(config)
    }

    private fun reconfigureOnHandler(newConfig: StreamConfig, generation: Long) {
        if (released) return
        if (closingCamera) {
            pendingStart = PendingStart(newConfig, generation)
            return
        }
        val old = currentConfig
        if (old == null || cameraDevice == null) {
            startOnHandler(newConfig, generation)
            return
        }

        if (!requiresPipelineRestart(old, newConfig)) {
            currentConfig = newConfig
            if (old.codec.isMediaCodec && old.bitrateKbps != newConfig.bitrateKbps) {
                encoder?.updateBitrate(newConfig.bitrateKbps)
            }
            if (old.torchEnabled != newConfig.torchEnabled) {
                applyRepeatingRequestOnHandler()
            }
            return
        }

        // Resolution/FPS/codec/camera/JPEG quality changed: close camera cleanly while keeping current
        // encoder surface alive until onClosed() drains the HAL, then reopen with the new encoder.
        startOnHandler(newConfig, generation)
    }

    private fun createEncoder(config: StreamConfig, generation: Long): VideoEncoder? = try {
        when (config.codec) {
            StreamCodec.H264, StreamCodec.H265, StreamCodec.AV1 -> MediaCodecVideoEncoder(
                codecType = config.codec,
                width = config.width,
                height = config.height,
                fps = config.fps,
                bitrateKbps = config.bitrateKbps,
                generation = generation,
                frameHub = frameHub,
                onFatalError = { err ->
                    postOnHandler {
                        val msg = "Ошибка кодировщика ${config.codec.displayName}: ${err.message ?: "неизвестная"}"
                        Log.e(TAG, msg, err)
                        pendingStart = null
                        stopInternalOnHandler(closeSubscribers = true)
                        onFatalError(msg)
                    }
                }
            )

            StreamCodec.MJPEG -> MjpegEncoder(
                width = config.width,
                height = config.height,
                jpegQuality = config.jpegQuality,
                generation = generation,
                frameHub = frameHub
            )
        }
    } catch (t: Throwable) {
        val msg = "Не удалось инициализировать кодек ${config.codec.displayName}: ${t.message}"
        Log.e(TAG, msg, t)
        onFatalError(msg)
        null
    }

    @SuppressLint("MissingPermission")
    private fun openCameraOnHandler(config: StreamConfig) {
        val epoch = ++openingEpoch
        try {
            val chars = cameraManager.getCameraCharacteristics(config.cameraId)
            activeFpsRange = CameraEnumerator.selectFpsRange(chars, config.fps)
            Log.i(
                TAG,
                "Opening camera=${config.cameraId} ${config.width}x${config.height}@${config.fps}fps " +
                    "codec=${config.codec.apiValue} fpsRange=$activeFpsRange preview=${previewSurface?.isValid == true}"
            )

            cameraManager.openCamera(
                config.cameraId,
                object : CameraDevice.StateCallback() {
                    override fun onOpened(camera: CameraDevice) {
                        if (epoch != openingEpoch || released) {
                            runCatching { camera.close() }
                            return
                        }
                        cameraDevice = camera
                        scheduleSessionRebuildOnHandler()
                    }

                    override fun onClosed(camera: CameraDevice) {
                        cameraHandler.removeCallbacks(closeTimeoutRunnable)
                        closingCamera = false
                        sessionPreviewSurface = null
                        // Only release the encoder & detached preview surfaces after CameraDevice drains!
                        stopEncoderOnlyOnHandler()
                        flushDeferredPreviewCleanups()
                        closeWaitLatch?.countDown()
                        closeWaitLatch = null
                        val next = pendingStart
                        pendingStart = null
                        if (next != null && !released) {
                            startOnHandler(next.config, next.generation)
                        }
                    }

                    override fun onDisconnected(camera: CameraDevice) {
                        Log.w(TAG, "CameraDevice.onDisconnected id=${camera.id}")
                        if (epoch != openingEpoch) {
                            runCatching { camera.close() }
                            return
                        }
                        pendingStart = null
                        handleCameraFailureOnHandler(camera, "Камера отключена системой")
                    }

                    override fun onError(camera: CameraDevice, error: Int) {
                        val cfg = currentConfig
                        Log.e(
                            TAG,
                            "CameraDevice.onError id=${camera.id} error=$error " +
                                "cfg=${cfg?.width}x${cfg?.height}@${cfg?.fps} codec=${cfg?.codec} " +
                                "fpsRange=$activeFpsRange preview=${previewSurface?.isValid}"
                        )
                        if (epoch != openingEpoch) {
                            runCatching { camera.close() }
                            return
                        }
                        if (error == ERROR_CAMERA_DEVICE &&
                            cfg != null &&
                            !released
                        ) {
                            if (lastConfiguredElapsedMs > 0L &&
                                SystemClock.elapsedRealtime() - lastConfiguredElapsedMs > 10_000L
                            ) {
                                deviceErrorRecoveryAttempts = 0
                            }
                            if (deviceErrorRecoveryAttempts < MAX_DEVICE_ERROR_RECOVERY_ATTEMPTS) {
                                deviceErrorRecoveryAttempts++
                                Log.w(
                                    TAG,
                                    "Recovering from CameraDevice ERROR_CAMERA_DEVICE (attempt #$deviceErrorRecoveryAttempts)"
                                )
                                val gen = frameHub.beginNewStream(closeExistingSubscribers = true)
                                if (cameraDevice == null) {
                                    cameraDevice = camera
                                }
                                pendingStart = PendingStart(cfg, gen)
                                stopInternalOnHandler(closeSubscribers = false)
                                return
                            }
                        }
                        pendingStart = null
                        handleCameraFailureOnHandler(camera, describeCameraError(error))
                    }
                },
                cameraHandler
            )
        } catch (se: SecurityException) {
            stopEncoderOnlyOnHandler()
            onFatalError("Нет разрешения на камеру: ${se.message}")
        } catch (cae: CameraAccessException) {
            stopEncoderOnlyOnHandler()
            onFatalError("Не удалось открыть камеру ${config.cameraId}: ${cae.message}")
        } catch (t: Throwable) {
            stopEncoderOnlyOnHandler()
            onFatalError("Ошибка открытия камеры ${config.cameraId}: ${t.message}")
        }
    }

    private fun scheduleSessionRebuildOnHandler() {
        val device = cameraDevice ?: return
        val enc = encoder ?: return
        if (closingCamera || released) return

        if (sessionConfiguring) {
            needsSessionRebuild = true
            return
        }

        if (!enc.inputSurface.isValid) {
            Log.e(TAG, "Encoder input surface became invalid before createCaptureSession")
            pendingStart = null
            stopInternalOnHandler(closeSubscribers = true)
            onFatalError("Поверхность кодировщика стала недействительной")
            return
        }

        sessionConfiguring = true
        needsSessionRebuild = false
        val previousSession = captureSession
        captureSession = null
        sessionPreviewSurface = null
        if (previousSession != null) {
            runCatching { previousSession.stopRepeating() }
            runCatching { previousSession.abortCaptures() }
        }

        val targets = mutableListOf(enc.inputSurface)
        val uiPreview = previewSurface?.takeIf { it.isValid }
        val previewForSession: Surface? = if (uiPreview != null) {
            scheduleDummyPreviewRelease()
            uiPreview
        } else if (currentConfig?.codec?.isMediaCodec == true) {
            ensureDummyPreviewSurface(currentConfig)
        } else {
            scheduleDummyPreviewRelease()
            null
        }
        if (previewForSession != null) {
            targets.add(previewForSession)
        }

        val outputs = targets.map { OutputConfiguration(it) }
        val epoch = openingEpoch

        val callback = object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(session: CameraCaptureSession) {
                sessionConfiguring = false
                if (epoch != openingEpoch || closingCamera || released || cameraDevice !== device || encoder !== enc) {
                    runCatching { session.close() }
                    return
                }
                sessionRecoveryAttempts = 0
                lastConfiguredElapsedMs = SystemClock.elapsedRealtime()
                captureSession = session
                sessionPreviewSurface = previewForSession
                // Old detached preview surfaces are no longer bound to any active session.
                flushDeferredPreviewCleanups()
                if (needsSessionRebuild) {
                    scheduleSessionRebuildOnHandler()
                } else {
                    applyRepeatingRequestOnHandler()
                }
            }

            override fun onConfigureFailed(session: CameraCaptureSession) {
                sessionConfiguring = false
                if (epoch != openingEpoch || closingCamera || released || cameraDevice !== device) {
                    return
                }
                Log.e(TAG, "CameraCaptureSession.onConfigureFailed (targets=${targets.size}, fpsRange=$activeFpsRange)")
                if (previewForSession != null) {
                    if (previewSurface === previewForSession) {
                        previewSurface = null
                    }
                    if (dummyPreviewReader?.surface === previewForSession) {
                        releaseDummyPreviewNow()
                    }
                    sessionPreviewSurface = null
                    needsSessionRebuild = false
                    flushDeferredPreviewCleanups()
                    // Retry without secondary surface if device rejected dual-surface config
                    createEncoderOnlySessionFallback(device, enc, epoch)
                    return
                }
                val cfg = currentConfig
                if (cfg != null && sessionRecoveryAttempts < MAX_SESSION_RECOVERY_ATTEMPTS) {
                    sessionRecoveryAttempts++
                    Log.w(TAG, "Attempting automatic camera reopen recovery #$sessionRecoveryAttempts after onConfigureFailed")
                    startOnHandler(cfg, frameHub.currentGeneration)
                    return
                }
                val msg = "Не удалось настроить сессию захвата камеры"
                pendingStart = null
                stopInternalOnHandler(closeSubscribers = true)
                onFatalError(msg)
            }
        }

        try {
            val sessionConfig = SessionConfiguration(
                SessionConfiguration.SESSION_REGULAR,
                outputs,
                executor,
                callback
            )
            device.createCaptureSession(sessionConfig)
        } catch (t: Throwable) {
            sessionConfiguring = false
            Log.e(TAG, "createCaptureSession threw", t)
            // After waitUntilIdle(-110) or HAL stream error, CameraDevice is wedged and must be reopened cleanly.
            val cfg = currentConfig
            if (cfg != null && sessionRecoveryAttempts < MAX_SESSION_RECOVERY_ATTEMPTS) {
                sessionRecoveryAttempts++
                Log.w(TAG, "Recovering from createCaptureSession error by reopening camera (attempt #$sessionRecoveryAttempts)")
                startOnHandler(cfg, frameHub.currentGeneration)
                return
            }
            pendingStart = null
            stopInternalOnHandler(closeSubscribers = true)
            onFatalError("Сбой запуска сессии камеры: ${t.message}")
        }
    }

    private fun applyRepeatingRequestOnHandler() {
        val device = cameraDevice ?: return
        val session = captureSession ?: return
        val enc = encoder ?: return
        val cfg = currentConfig ?: return
        if (closingCamera || released || sessionConfiguring) return

        try {
            if (!enc.inputSurface.isValid) return
            val builder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(enc.inputSurface)
                val configuredPreview = sessionPreviewSurface
                if (configuredPreview != null && configuredPreview.isValid) {
                    addTarget(configuredPreview)
                }
                set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO)
                set(
                    CaptureRequest.CONTROL_AF_MODE,
                    CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO
                )
                activeFpsRange?.let { set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }

                val flashMode = if (cfg.torchEnabled) {
                    CameraMetadata.FLASH_MODE_TORCH
                } else {
                    CameraMetadata.FLASH_MODE_OFF
                }
                set(CaptureRequest.FLASH_MODE, flashMode)
            }
            session.setRepeatingRequest(builder.build(), null, cameraHandler)
        } catch (iae: IllegalArgumentException) {
            Log.w(TAG, "applyRepeatingRequest had stale surface, rebuilding session", iae)
            sessionPreviewSurface = null
            scheduleSessionRebuildOnHandler()
        } catch (t: Throwable) {
            Log.w(TAG, "applyRepeatingRequest failed", t)
        }
    }

    private fun handleCameraFailureOnHandler(camera: CameraDevice, message: String) {
        if (cameraDevice == null) {
            cameraDevice = camera
        }
        stopInternalOnHandler(closeSubscribers = true)
        onFatalError(message)
    }

    private fun stopInternalOnHandler(closeSubscribers: Boolean) {
        openingEpoch++
        sessionConfiguring = false
        needsSessionRebuild = false
        sessionPreviewSurface = null
        if (closeSubscribers) {
            frameHub.closeAllSubscribers()
        }

        runCatching { captureSession?.stopRepeating() }
        runCatching { captureSession?.abortCaptures() }
        captureSession = null

        val cam = cameraDevice
        cameraDevice = null
        if (cam != null) {
            closingCamera = true
            cameraHandler.removeCallbacks(closeTimeoutRunnable)
            cameraHandler.postDelayed(closeTimeoutRunnable, 1500L)
            // Keep encoder surface alive until onClosed(cam) fires so Camera HAL drains without timeout.
            runCatching { cam.close() }.onFailure {
                cameraHandler.removeCallbacks(closeTimeoutRunnable)
                closingCamera = false
                stopEncoderOnlyOnHandler()
                flushDeferredPreviewCleanups()
                closeWaitLatch?.countDown()
                closeWaitLatch = null
            }
        } else {
            stopEncoderOnlyOnHandler()
            flushDeferredPreviewCleanups()
        }

        currentConfig = null
        activeFpsRange = null
    }

    private fun stopEncoderOnlyOnHandler() {
        frameHub.onRequestSyncFrame = null
        runCatching { encoder?.stopAndRelease() }
        encoder = null
        releaseDummyPreviewNow()
    }

    private fun ensureDummyPreviewSurface(config: StreamConfig?): Surface? {
        val existing = dummyPreviewReader
        if (existing != null && existing.surface.isValid) {
            return existing.surface
        }
        releaseDummyPreviewNow()
        val w = if ((config?.width ?: 1280) * 3 == (config?.height ?: 720) * 4) 640 else 640
        val h = if ((config?.width ?: 1280) * 3 == (config?.height ?: 720) * 4) 480 else 360
        return runCatching {
            val reader = ImageReader.newInstance(
                w,
                h,
                ImageFormat.PRIVATE,
                3,
                HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE
            )
            reader.setOnImageAvailableListener({ r ->
                runCatching { r.acquireLatestImage()?.close() }
            }, cameraHandler)
            dummyPreviewReader = reader
            reader.surface
        }.onFailure { err ->
            Log.w(TAG, "Could not create background PRIVATE preview sink: ${err.message}")
        }.getOrNull()
    }

    private fun scheduleDummyPreviewRelease() {
        val reader = dummyPreviewReader ?: return
        dummyPreviewReader = null
        deferredPreviewCleanups.add {
            runCatching { reader.setOnImageAvailableListener(null, null) }
            runCatching { reader.close() }
        }
    }

    private fun releaseDummyPreviewNow() {
        val reader = dummyPreviewReader ?: return
        dummyPreviewReader = null
        runCatching { reader.setOnImageAvailableListener(null, null) }
        runCatching { reader.close() }
    }

    private fun createEncoderOnlySessionFallback(
        device: CameraDevice,
        enc: VideoEncoder,
        epoch: Int
    ) {
        if (closingCamera || released || cameraDevice !== device || encoder !== enc) return
        sessionConfiguring = true
        val outputs = listOf(OutputConfiguration(enc.inputSurface))
        val cb = object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(session: CameraCaptureSession) {
                sessionConfiguring = false
                if (epoch != openingEpoch || closingCamera || released || cameraDevice !== device || encoder !== enc) {
                    runCatching { session.close() }
                    return
                }
                sessionRecoveryAttempts = 0
                lastConfiguredElapsedMs = SystemClock.elapsedRealtime()
                captureSession = session
                sessionPreviewSurface = null
                flushDeferredPreviewCleanups()
                applyRepeatingRequestOnHandler()
            }

            override fun onConfigureFailed(session: CameraCaptureSession) {
                sessionConfiguring = false
                if (epoch != openingEpoch || closingCamera || released || cameraDevice !== device) return
                val cfg = currentConfig
                if (cfg != null && sessionRecoveryAttempts < MAX_SESSION_RECOVERY_ATTEMPTS) {
                    sessionRecoveryAttempts++
                    startOnHandler(cfg, frameHub.currentGeneration)
                    return
                }
                pendingStart = null
                stopInternalOnHandler(closeSubscribers = true)
                onFatalError("Не удалось настроить сессию захвата камеры")
            }
        }
        try {
            device.createCaptureSession(
                SessionConfiguration(SessionConfiguration.SESSION_REGULAR, outputs, executor, cb)
            )
        } catch (t: Throwable) {
            sessionConfiguring = false
            pendingStart = null
            stopInternalOnHandler(closeSubscribers = true)
            onFatalError("Сбой запуска сессии камеры: ${t.message}")
        }
    }

    private fun flushDeferredPreviewCleanups() {
        if (deferredPreviewCleanups.isEmpty()) return
        val copy = deferredPreviewCleanups.toList()
        deferredPreviewCleanups.clear()
        for (cleanup in copy) {
            runCatching { cleanup() }
        }
    }

    private fun describeCameraError(error: Int): String = when (error) {
        CameraDevice.StateCallback.ERROR_CAMERA_IN_USE ->
            "Камера занята другим приложением (код 1)"

        CameraDevice.StateCallback.ERROR_MAX_CAMERAS_IN_USE ->
            "Достигнут лимит одновременно открытых камер (код 2)"

        CameraDevice.StateCallback.ERROR_CAMERA_DISABLED ->
            "Камера отключена политикой устройства (код 3)"

        CameraDevice.StateCallback.ERROR_CAMERA_DEVICE ->
            "Аппаратный сбой камеры (код 4). Попробуйте снизить разрешение/FPS или выбрать MJPEG"

        CameraDevice.StateCallback.ERROR_CAMERA_SERVICE ->
            "Сбой системного сервиса камеры (код 5)"

        else -> "Ошибка Camera2 (код $error)"
    }

    companion object {
        private const val TAG = "CameraStreamController"
        private const val MAX_SESSION_RECOVERY_ATTEMPTS = 2
        private const val MAX_DEVICE_ERROR_RECOVERY_ATTEMPTS = 2

        /**
         * Returns true if switching from [old] to [new] requires tearing down and recreating
         * the Camera2 session / encoder (and advancing [FrameHub] stream generation).
         */
        fun requiresPipelineRestart(old: StreamConfig, new: StreamConfig): Boolean {
            if (old.cameraId != new.cameraId) return true
            if (old.width != new.width || old.height != new.height) return true
            if (old.fps != new.fps) return true
            if (old.codec != new.codec) return true
            if (new.codec == StreamCodec.MJPEG && old.jpegQuality != new.jpegQuality) return true
            return false
        }
    }
}
