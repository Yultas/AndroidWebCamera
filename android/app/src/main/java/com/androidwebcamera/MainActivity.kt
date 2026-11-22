package com.androidwebcamera

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.getValue
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.androidwebcamera.service.StreamRepository
import com.androidwebcamera.ui.StreamScreen
import com.androidwebcamera.ui.theme.AndroidWebCameraTheme

/**
 * Main Material 3 host activity. Does NOT set FLAG_KEEP_SCREEN_ON so the user can
 * freely lock the phone while [com.androidwebcamera.service.CameraStreamService] continues streaming.
 */
class MainActivity : ComponentActivity() {

    private var pendingStartAfterCameraPermission = false
    private var pendingHotspotAfterWifiPermission = false

    private val cameraAndNotifyLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        val cameraGranted = results[Manifest.permission.CAMERA] == true ||
            hasPermission(Manifest.permission.CAMERA)
        if (cameraGranted) {
            StreamRepository.initialize(this)
            if (pendingStartAfterCameraPermission) {
                pendingStartAfterCameraPermission = false
                StreamRepository.startStreaming(this)
            }
        } else {
            pendingStartAfterCameraPermission = false
        }
    }

    private val hotspotPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        val granted = results.values.any { it } || hasHotspotPermission()
        if (granted && pendingHotspotAfterWifiPermission) {
            pendingHotspotAfterWifiPermission = false
            StreamRepository.toggleHotspot(this)
        } else {
            pendingHotspotAfterWifiPermission = false
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        StreamRepository.initialize(this)
        requestStartupPermissionsIfNeeded()

        setContent {
            AndroidWebCameraTheme {
                val state by StreamRepository.state.collectAsStateWithLifecycle()
                StreamScreen(
                    state = state,
                    onStartStopClick = {
                        if (state.isStreaming) {
                            StreamRepository.stopStreaming(this)
                        } else {
                            ensureCameraPermissionAndStart()
                        }
                    },
                    onUpdateConfig = { transform -> StreamRepository.updateConfig(transform) },
                    onToggleHotspot = { ensureHotspotPermissionAndToggle() },
                    onRefreshNetwork = { StreamRepository.refreshEndpoints() },
                    onAttachSurface = { surface -> StreamRepository.attachPreviewSurface(surface) },
                    onDetachSurface = { surface, onSafeToRelease ->
                        StreamRepository.detachPreviewSurface(surface, onSafeToRelease)
                    },
                    onDismissError = { StreamRepository.clearError() }
                )
            }
        }
    }

    override fun onResume() {
        super.onResume()
        StreamRepository.refreshEndpoints()
    }

    private fun requestStartupPermissionsIfNeeded() {
        val needed = mutableListOf<String>()
        if (!hasPermission(Manifest.permission.CAMERA)) {
            needed.add(Manifest.permission.CAMERA)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            !hasPermission(Manifest.permission.POST_NOTIFICATIONS)
        ) {
            needed.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (needed.isNotEmpty()) {
            cameraAndNotifyLauncher.launch(needed.toTypedArray())
        }
    }

    private fun ensureCameraPermissionAndStart() {
        if (hasPermission(Manifest.permission.CAMERA)) {
            StreamRepository.startStreaming(this)
        } else {
            pendingStartAfterCameraPermission = true
            val needed = buildList {
                add(Manifest.permission.CAMERA)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                    !hasPermission(Manifest.permission.POST_NOTIFICATIONS)
                ) {
                    add(Manifest.permission.POST_NOTIFICATIONS)
                }
            }
            cameraAndNotifyLauncher.launch(needed.toTypedArray())
        }
    }

    private fun ensureHotspotPermissionAndToggle() {
        if (hasHotspotPermission()) {
            StreamRepository.toggleHotspot(this)
        } else {
            pendingHotspotAfterWifiPermission = true
            val perms = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                arrayOf(Manifest.permission.NEARBY_WIFI_DEVICES)
            } else {
                arrayOf(
                    Manifest.permission.ACCESS_FINE_LOCATION,
                    Manifest.permission.ACCESS_COARSE_LOCATION
                )
            }
            hotspotPermissionLauncher.launch(perms)
        }
    }

    private fun hasHotspotPermission(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            hasPermission(Manifest.permission.NEARBY_WIFI_DEVICES)
        } else {
            hasPermission(Manifest.permission.ACCESS_FINE_LOCATION) ||
                hasPermission(Manifest.permission.ACCESS_COARSE_LOCATION)
        }

    private fun hasPermission(permission: String): Boolean =
        ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED
}
