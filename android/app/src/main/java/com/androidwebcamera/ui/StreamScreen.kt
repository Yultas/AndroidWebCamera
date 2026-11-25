package com.androidwebcamera.ui

import android.content.Intent
import android.net.Uri
import android.os.PowerManager
import android.provider.Settings
import android.view.Surface
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.BatteryAlert
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Stop
import androidx.compose.material.icons.rounded.Visibility
import androidx.compose.material.icons.rounded.VisibilityOff
import androidx.compose.material.icons.rounded.Warning
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.androidwebcamera.model.StreamConfig
import com.androidwebcamera.model.StreamState
import com.androidwebcamera.ui.components.CameraPreviewCard
import com.androidwebcamera.ui.components.ConnectionCard
import com.androidwebcamera.ui.components.StreamControlsCard
import kotlinx.coroutines.launch

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun StreamScreen(
    state: StreamState,
    onStartStopClick: () -> Unit,
    onUpdateConfig: ((StreamConfig) -> StreamConfig) -> Unit,
    onToggleHotspot: () -> Unit,
    onRefreshNetwork: () -> Unit,
    onAttachSurface: (Surface) -> Unit,
    onDetachSurface: (Surface, (() -> Unit)?) -> Unit,
    onDismissError: () -> Unit
) {
    val context = LocalContext.current
    val snackbarHostState = remember { SnackbarHostState() }
    val uiScope = rememberCoroutineScope()
    var showPreview by remember { mutableStateOf(true) }
    var dismissBatteryHint by remember { mutableStateOf(false) }

    val powerManager = remember(context) {
        context.getSystemService(PowerManager::class.java)
    }
    var batteryCheckTick by remember { mutableStateOf(0) }
    val lifecycleOwner = androidx.lifecycle.compose.LocalLifecycleOwner.current
    androidx.compose.runtime.DisposableEffect(lifecycleOwner) {
        val observer = androidx.lifecycle.LifecycleEventObserver { _, event ->
            if (event == androidx.lifecycle.Lifecycle.Event.ON_RESUME) {
                batteryCheckTick++
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
    val isIgnoringBatteryOpt = remember(context, state.isStreaming, batteryCheckTick) {
        powerManager?.isIgnoringBatteryOptimizations(context.packageName) ?: true
    }

    LaunchedEffect(state.errorMessage) {
        val msg = state.errorMessage
        if (!msg.isNullOrBlank()) {
            snackbarHostState.showSnackbar(msg)
            onDismissError()
        }
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbarHostState) },
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text(
                            text = "AndroidWebCamera",
                            fontWeight = FontWeight.SemiBold
                        )
                        Text(
                            text = if (state.isStreaming) {
                                "В эфире • Клиентов: ${state.connectedClients}"
                            } else {
                                "Ожидание запуска"
                            },
                            style = MaterialTheme.typography.labelMedium,
                            color = if (state.isStreaming) {
                                MaterialTheme.colorScheme.primary
                            } else {
                                MaterialTheme.colorScheme.onSurfaceVariant
                            }
                        )
                    }
                },
                actions = {
                    AssistChip(
                        onClick = { showPreview = !showPreview },
                        label = { Text(if (showPreview) "Превью" else "Скрыто") },
                        leadingIcon = {
                            Icon(
                                imageVector = if (showPreview) Icons.Rounded.Visibility else Icons.Rounded.VisibilityOff,
                                contentDescription = null
                            )
                        },
                        colors = AssistChipDefaults.assistChipColors()
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface
                )
            )
        },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                onClick = onStartStopClick,
                containerColor = if (state.isStreaming) {
                    MaterialTheme.colorScheme.errorContainer
                } else {
                    MaterialTheme.colorScheme.primaryContainer
                },
                contentColor = if (state.isStreaming) {
                    MaterialTheme.colorScheme.onErrorContainer
                } else {
                    MaterialTheme.colorScheme.onPrimaryContainer
                },
                icon = {
                    Icon(
                        imageVector = if (state.isStreaming) Icons.Rounded.Stop else Icons.Rounded.PlayArrow,
                        contentDescription = null
                    )
                },
                text = {
                    Text(
                        text = if (state.isStreaming) "Остановить" else "Начать трансляцию",
                        fontWeight = FontWeight.SemiBold
                    )
                }
            )
        }
    ) { innerPadding ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding),
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 96.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            if (!isIgnoringBatteryOpt && !dismissBatteryHint) {
                item(key = "battery_banner") {
                    BatteryOptimizationBanner(
                        onOpenSettings = {
                            val pkgUri = Uri.fromParts("package", context.packageName, null)
                            val directRequestIntent = Intent(
                                Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                                pkgUri
                            )
                            runCatching { context.startActivity(directRequestIntent) }
                                .recoverCatching {
                                    context.startActivity(
                                        Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
                                    )
                                }
                                .recoverCatching {
                                    context.startActivity(
                                        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, pkgUri)
                                    )
                                }
                        },
                        onDismiss = { dismissBatteryHint = true }
                    )
                }
            }

            if (state.availableCameras.isEmpty()) {
                item(key = "no_camera") {
                    Card(
                        shape = RoundedCornerShape(20.dp),
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.errorContainer
                        )
                    ) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.padding(16.dp)
                        ) {
                            Icon(Icons.Rounded.Warning, contentDescription = null)
                            Spacer(modifier = Modifier.width(12.dp))
                            Text(
                                text = "Не удалось обнаружить камеры или не выдано разрешение CAMERA.",
                                style = MaterialTheme.typography.bodyMedium
                            )
                        }
                    }
                }
            }

            item(key = "preview") {
                CameraPreviewCard(
                    state = state,
                    showPreview = showPreview,
                    onAttachSurface = onAttachSurface,
                    onDetachSurface = onDetachSurface
                )
            }

            item(key = "connection") {
                ConnectionCard(
                    state = state,
                    onToggleHotspot = onToggleHotspot,
                    onRefreshNetwork = onRefreshNetwork,
                    onCopiedUrl = { url ->
                        uiScope.launch {
                            snackbarHostState.showSnackbar("Скопировано: $url")
                        }
                    }
                )
            }

            item(key = "controls") {
                StreamControlsCard(
                    state = state,
                    onUpdateConfig = onUpdateConfig
                )
            }
        }
    }
}

@Composable
private fun BatteryOptimizationBanner(
    onOpenSettings: () -> Unit,
    onDismiss: () -> Unit
) {
    Card(
        onClick = onOpenSettings,
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.tertiaryContainer
        ),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(modifier = Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    imageVector = Icons.Rounded.BatteryAlert,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onTertiaryContainer
                )
                Spacer(modifier = Modifier.width(10.dp))
                Text(
                    text = "Рекомендуется для долгой работы при выключенном экране",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onTertiaryContainer
                )
            }
            Text(
                text = "Разрешите приложению работу в фоне без ограничений энергосбережения, чтобы система не усыпляла поток при длительной блокировке.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onTertiaryContainer,
                modifier = Modifier.padding(top = 6.dp)
            )
            Row(
                horizontalArrangement = Arrangement.End,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 6.dp)
            ) {
                TextButton(onClick = onDismiss) {
                    Text("Скрыть")
                }
                TextButton(onClick = onOpenSettings) {
                    Text("Настроить")
                }
            }
        }
    }
}
