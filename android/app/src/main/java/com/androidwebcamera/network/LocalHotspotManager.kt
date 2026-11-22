package com.androidwebcamera.network

import android.annotation.SuppressLint
import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.androidwebcamera.model.HotspotState
import java.nio.charset.StandardCharsets

/**
 * Manages an Android Local-Only Wi-Fi Hotspot so a PC can connect directly to the phone
 * without any external router or root access.
 */
class LocalHotspotManager(context: Context) {

    private val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private val mainHandler = Handler(Looper.getMainLooper())
    private var reservation: WifiManager.LocalOnlyHotspotReservation? = null

    @SuppressLint("MissingPermission")
    fun start(onStateChanged: (HotspotState) -> Unit) {
        if (reservation != null) return
        onStateChanged(HotspotState.Starting)

        try {
            wifiManager.startLocalOnlyHotspot(
                object : WifiManager.LocalOnlyHotspotCallback() {
                    override fun onStarted(res: WifiManager.LocalOnlyHotspotReservation) {
                        reservation = res
                        val cfg = res.softApConfiguration
                        val ssid = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            cfg?.wifiSsid?.bytes?.toString(StandardCharsets.UTF_8)
                        } else {
                            @Suppress("DEPRECATION")
                            cfg?.ssid
                        }.orEmpty().ifBlank { "AndroidAP" }
                        val password = cfg?.passphrase.orEmpty()
                        onStateChanged(HotspotState.Active(ssid = ssid, passphrase = password))
                    }

                    override fun onStopped() {
                        reservation = null
                        onStateChanged(HotspotState.Inactive)
                    }

                    override fun onFailed(reason: Int) {
                        reservation = null
                        val description = when (reason) {
                            ERROR_TETHERING_DISALLOWED ->
                                "Точка доступа запрещена политикой устройства"
                            ERROR_INCOMPATIBLE_MODE ->
                                "Отключите системную точку доступа перед запуском локальной"
                            ERROR_NO_CHANNEL ->
                                "Нет свободного Wi-Fi канала"
                            else -> "Не удалось запустить точку доступа (код $reason)"
                        }
                        onStateChanged(HotspotState.Error(description))
                    }
                },
                mainHandler
            )
        } catch (se: SecurityException) {
            onStateChanged(HotspotState.Error("Нет разрешения на управление точкой доступа Wi-Fi"))
        } catch (t: Throwable) {
            onStateChanged(HotspotState.Error(t.message ?: "Ошибка запуска точки доступа"))
        }
    }

    fun stop(onStateChanged: (HotspotState) -> Unit) {
        runCatching { reservation?.close() }
        reservation = null
        onStateChanged(HotspotState.Inactive)
    }
}
