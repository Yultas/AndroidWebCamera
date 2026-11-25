package com.androidwebcamera.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.Network
import android.net.wifi.WifiManager
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import com.androidwebcamera.MainActivity
import com.androidwebcamera.R
import com.androidwebcamera.camera.CameraStreamController
import com.androidwebcamera.model.HotspotState
import com.androidwebcamera.model.StreamConfig
import com.androidwebcamera.network.LocalHotspotManager
import com.androidwebcamera.network.NetworkAddressProvider
import com.androidwebcamera.server.FrameHub
import com.androidwebcamera.server.StreamHttpServer
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch

/**
 * Foreground camera streaming service that keeps Camera2, video encoding, and the embedded HTTP/WS
 * server running smoothly even when the screen turns off and the device is locked.
 */
class CameraStreamService : Service() {

    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    private lateinit var frameHub: FrameHub
    private lateinit var cameraController: CameraStreamController
    private lateinit var httpServer: StreamHttpServer
    private lateinit var hotspotManager: LocalHotspotManager
    private lateinit var notificationManager: NotificationManager
    private lateinit var connectivityManager: ConnectivityManager

    private var partialWakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var networkCallback: ConnectivityManager.NetworkCallback? = null

    @Volatile
    private var pendingPreviewSurface: android.view.Surface? = null

    override fun onCreate() {
        super.onCreate()
        StreamRepository.bindService(this)
        if (StreamRepository.state.value.availableCameras.isEmpty()) {
            StreamRepository.initialize(this)
        }

        notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        connectivityManager = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        ensureNotificationChannel()

        frameHub = FrameHub()
        cameraController = CameraStreamController(
            context = this,
            frameHub = frameHub,
            onFatalError = { message ->
                serviceScope.launch {
                    StreamRepository.mutateState { it.copy(errorMessage = message) }
                    stopStreamingPipeline()
                    stopSelf()
                }
            },
            onConfigFallback = { fallbackCfg, notice ->
                serviceScope.launch {
                    StreamRepository.mutateState {
                        it.copy(
                            config = fallbackCfg,
                            errorMessage = notice
                        )
                    }
                }
            }
        )
        httpServer = StreamHttpServer(
            context = this,
            frameHub = frameHub,
            stateProvider = { StreamRepository.state.value },
            onUpdateConfig = { updated ->
                StreamRepository.updateConfig { updated }
                StreamRepository.state.value.config
            }
        )
        hotspotManager = LocalHotspotManager(this)

        observeMetrics()
        registerNetworkWatcher()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopFromUi()
                return START_NOT_STICKY
            }

            ACTION_TOGGLE_HOTSPOT -> {
                toggleHotspot()
                return START_STICKY
            }

            ACTION_START, null -> {
                startStreamingPipeline()
                return START_STICKY
            }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        unregisterNetworkWatcher()
        stopStreamingPipeline()
        cameraController.release()
        hotspotManager.stop { state ->
            StreamRepository.mutateState { it.copy(hotspotState = state) }
        }
        StreamRepository.unbindService(this)
        serviceScope.cancel()
        super.onDestroy()
    }

    fun stopFromUi() {
        stopStreamingPipeline()
        stopSelf()
    }

    fun applyLiveConfig(previousConfig: StreamConfig, newConfig: StreamConfig) {
        if (!StreamRepository.state.value.isStreaming) return
        val needsRestart = CameraStreamController.requiresPipelineRestart(previousConfig, newConfig)
        val gen = if (needsRestart) {
            // Synchronously close old subscribers and advance generation BEFORE any HTTP response returns
            // so newly connecting clients never receive stale frames or get killed mid-reconnect.
            frameHub.beginNewStream(closeExistingSubscribers = true)
        } else {
            frameHub.currentGeneration
        }

        cameraController.reconfigure(newConfig, gen)

        if (previousConfig.port != newConfig.port) {
            serviceScope.launch {
                runCatching { httpServer.start(newConfig.port) }.onFailure { err ->
                    StreamRepository.mutateState {
                        it.copy(errorMessage = "Не удалось открыть порт ${newConfig.port}: ${err.message}")
                    }
                }
            }
        }
        updateNotification()
    }

    fun attachPreviewSurface(surface: android.view.Surface) {
        pendingPreviewSurface = surface
        cameraController.attachPreviewSurface(surface)
    }

    fun detachPreviewSurface(surface: android.view.Surface, onSafeToRelease: (() -> Unit)? = null) {
        if (pendingPreviewSurface === surface) {
            pendingPreviewSurface = null
        }
        cameraController.detachPreviewSurface(surface, onSafeToRelease)
    }

    fun toggleHotspot() {
        val current = StreamRepository.state.value.hotspotState
        if (current is HotspotState.Active || current is HotspotState.Starting) {
            hotspotManager.stop { next ->
                StreamRepository.mutateState {
                    it.copy(
                        hotspotState = next,
                        endpoints = NetworkAddressProvider.getEndpoints(it.config.port)
                    )
                }
            }
        } else {
            hotspotManager.start { next ->
                StreamRepository.mutateState {
                    it.copy(
                        hotspotState = next,
                        endpoints = NetworkAddressProvider.getEndpoints(it.config.port)
                    )
                }
            }
        }
    }

    private fun startStreamingPipeline() {
        val state = StreamRepository.state.value
        val config = state.config

        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            buildNotification(),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
        )

        acquireWakeLocks()

        StreamRepository.mutateState {
            it.copy(
                isStarting = true,
                errorMessage = null,
                endpoints = NetworkAddressProvider.getEndpoints(config.port)
            )
        }

        try {
            val generation = frameHub.beginNewStream(closeExistingSubscribers = false)
            httpServer.start(config.port)
            cameraController.start(config, generation)
            pendingPreviewSurface?.let { surf ->
                if (surf.isValid) {
                    cameraController.attachPreviewSurface(surf)
                }
            }
            StreamRepository.mutateState { it.copy(isStreaming = true, isStarting = false) }
            updateNotification()
        } catch (t: Throwable) {
            releaseWakeLocks()
            httpServer.stop()
            cameraController.stop()
            StreamRepository.mutateState {
                it.copy(
                    isStreaming = false,
                    isStarting = false,
                    errorMessage = "Не удалось запустить трансляцию: ${t.message}"
                )
            }
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
    }

    private fun stopStreamingPipeline() {
        cameraController.stop()
        httpServer.stop()
        frameHub.beginNewStream(closeExistingSubscribers = true)
        releaseWakeLocks()
        StreamRepository.mutateState {
            it.copy(
                isStreaming = false,
                isStarting = false,
                connectedClients = 0,
                measuredFps = 0
            )
        }
        runCatching { stopForeground(STOP_FOREGROUND_REMOVE) }
    }

    private fun observeMetrics() {
        serviceScope.launch {
            frameHub.clientCount.collectLatest { count ->
                StreamRepository.mutateState { it.copy(connectedClients = count) }
                if (StreamRepository.state.value.isStreaming) {
                    updateNotification()
                }
            }
        }
        serviceScope.launch {
            frameHub.measuredFps.collectLatest { fps ->
                StreamRepository.mutateState { it.copy(measuredFps = fps) }
            }
        }
    }

    private fun acquireWakeLocks() {
        if (partialWakeLock == null) {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            // PARTIAL_WAKE_LOCK keeps CPU running for Camera2 + MediaCodec while allowing the screen to lock.
            partialWakeLock = pm.newWakeLock(
                PowerManager.PARTIAL_WAKE_LOCK,
                "AndroidWebCamera::StreamCpuLock"
            ).apply {
                setReferenceCounted(false)
                acquire(12 * 60 * 60 * 1000L)
            }
        }
        if (wifiLock == null) {
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            wifiLock = wm.createWifiLock(
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY,
                "AndroidWebCamera::StreamWifiLock"
            ).apply {
                setReferenceCounted(false)
                acquire()
            }
        }
    }

    private fun releaseWakeLocks() {
        runCatching {
            if (partialWakeLock?.isHeld == true) {
                partialWakeLock?.release()
            }
        }
        partialWakeLock = null

        runCatching {
            if (wifiLock?.isHeld == true) {
                wifiLock?.release()
            }
        }
        wifiLock = null
    }

    private fun registerNetworkWatcher() {
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) = refreshNetwork()
            override fun onLost(network: Network) = refreshNetwork()
        }
        networkCallback = cb
        runCatching { connectivityManager.registerDefaultNetworkCallback(cb) }
    }

    private fun unregisterNetworkWatcher() {
        networkCallback?.let { cb ->
            runCatching { connectivityManager.unregisterNetworkCallback(cb) }
        }
        networkCallback = null
    }

    private fun refreshNetwork() {
        serviceScope.launch {
            StreamRepository.refreshEndpoints()
            if (StreamRepository.state.value.isStreaming) {
                updateNotification()
            }
        }
    }

    private fun ensureNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.notification_channel_name),
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = getString(R.string.notification_channel_desc)
            setShowBadge(false)
        }
        notificationManager.createNotificationChannel(channel)
    }

    private fun updateNotification() {
        runCatching { notificationManager.notify(NOTIFICATION_ID, buildNotification()) }
    }

    private fun buildNotification(): Notification {
        val state = StreamRepository.state.value
        val cfg = state.config
        val primaryUrl = state.endpoints.firstOrNull { !it.isUsb }?.url
            ?: state.endpoints.firstOrNull()?.url
            ?: "http://127.0.0.1:${cfg.port}/"

        val openAppIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val stopIntent = PendingIntent.getService(
            this,
            1,
            Intent(this, CameraStreamService::class.java).apply { action = ACTION_STOP },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val contentText = "${cfg.width}×${cfg.height} @ ${cfg.fps}fps (${cfg.codec.apiValue.uppercase()}) • Клиентов: ${state.connectedClients} • $primaryUrl"

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setContentTitle(getString(R.string.notification_title_streaming))
            .setContentText(contentText)
            .setStyle(NotificationCompat.BigTextStyle().bigText(contentText))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setContentIntent(openAppIntent)
            .addAction(0, getString(R.string.action_stop), stopIntent)
            .build()
    }

    companion object {
        const val ACTION_START = "com.androidwebcamera.action.START"
        const val ACTION_STOP = "com.androidwebcamera.action.STOP"
        const val ACTION_TOGGLE_HOTSPOT = "com.androidwebcamera.action.TOGGLE_HOTSPOT"

        private const val CHANNEL_ID = "camera_stream_channel"
        private const val NOTIFICATION_ID = 1001
    }
}
