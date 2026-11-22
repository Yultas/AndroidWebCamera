package com.androidwebcamera.service

import android.content.Context
import android.content.Intent
import android.view.Surface
import androidx.core.content.ContextCompat
import com.androidwebcamera.camera.CameraEnumerator
import com.androidwebcamera.camera.CodecCapabilityDetector
import com.androidwebcamera.model.StreamConfig
import com.androidwebcamera.model.StreamState
import com.androidwebcamera.network.NetworkAddressProvider
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.lang.ref.WeakReference

/**
 * Process-scoped state holder and command bridge between Compose UI and [CameraStreamService].
 * Keeps UI state synchronized across screen locks, Activity recreation, and background streaming.
 */
object StreamRepository {

    private val _state = MutableStateFlow(StreamState())
    val state: StateFlow<StreamState> = _state.asStateFlow()

    @Volatile
    private var serviceRef: WeakReference<CameraStreamService>? = null

    fun initialize(context: Context) {
        val cameras = CameraEnumerator.enumerate(context)
        val codecs = CodecCapabilityDetector.detectSupportedCodecs()
        _state.update { current ->
            val selectedCam = cameras.firstOrNull { it.id == current.config.cameraId }
                ?: cameras.firstOrNull()
            val normalizedConfig = current.config.normalizedFor(selectedCam, codecs)
            current.copy(
                availableCameras = cameras,
                activeCamera = selectedCam,
                supportedCodecs = codecs,
                config = normalizedConfig,
                endpoints = NetworkAddressProvider.getEndpoints(normalizedConfig.port)
            )
        }
    }

    fun startStreaming(context: Context) {
        val intent = Intent(context, CameraStreamService::class.java).apply {
            action = CameraStreamService.ACTION_START
        }
        ContextCompat.startForegroundService(context, intent)
    }

    fun stopStreaming(context: Context) {
        val svc = serviceRef?.get()
        if (svc != null) {
            svc.stopFromUi()
            return
        }
        if (_state.value.isStreaming || _state.value.isStarting) {
            val intent = Intent(context, CameraStreamService::class.java).apply {
                action = CameraStreamService.ACTION_STOP
            }
            runCatching { context.startService(intent) }
        }
        _state.update {
            it.copy(
                isStreaming = false,
                isStarting = false,
                connectedClients = 0,
                measuredFps = 0
            )
        }
    }

    fun updateConfig(transform: (StreamConfig) -> StreamConfig) {
        var previousConfig: StreamConfig? = null
        var appliedConfig: StreamConfig? = null
        _state.update { current ->
            previousConfig = current.config
            val proposed = transform(current.config)
            val cam = current.availableCameras.firstOrNull { it.id == proposed.cameraId }
                ?: current.activeCamera
            val normalized = proposed.normalizedFor(cam, current.supportedCodecs)
            appliedConfig = normalized
            current.copy(
                config = normalized,
                activeCamera = cam,
                endpoints = NetworkAddressProvider.getEndpoints(normalized.port),
                errorMessage = null
            )
        }
        val prev = previousConfig
        val next = appliedConfig
        if (prev != null && next != null) {
            serviceRef?.get()?.applyLiveConfig(prev, next)
        }
    }

    fun toggleHotspot(context: Context) {
        val svc = serviceRef?.get()
        if (svc != null) {
            svc.toggleHotspot()
        } else {
            val intent = Intent(context, CameraStreamService::class.java).apply {
                action = CameraStreamService.ACTION_TOGGLE_HOTSPOT
            }
            runCatching { context.startService(intent) }
        }
    }

    fun refreshEndpoints() {
        _state.update { current ->
            current.copy(endpoints = NetworkAddressProvider.getEndpoints(current.config.port))
        }
    }

    fun attachPreviewSurface(surface: Surface) {
        serviceRef?.get()?.attachPreviewSurface(surface)
    }

    fun detachPreviewSurface(surface: Surface, onSafeToRelease: (() -> Unit)? = null) {
        val svc = serviceRef?.get()
        if (svc != null) {
            svc.detachPreviewSurface(surface, onSafeToRelease)
        } else {
            onSafeToRelease?.let { runCatching { it() } }
        }
    }

    fun clearError() {
        _state.update { it.copy(errorMessage = null) }
    }

    internal fun bindService(service: CameraStreamService) {
        serviceRef = WeakReference(service)
    }

    internal fun unbindService(service: CameraStreamService) {
        if (serviceRef?.get() === service) {
            serviceRef = null
        }
    }

    internal fun mutateState(block: (StreamState) -> StreamState) {
        _state.update(block)
    }
}
