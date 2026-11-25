package com.androidwebcamera.ui.components

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.RotateRight
import androidx.compose.material.icons.rounded.Cameraswitch
import androidx.compose.material.icons.rounded.FlashOff
import androidx.compose.material.icons.rounded.FlashOn
import androidx.compose.material.icons.rounded.Flip
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.androidwebcamera.model.CameraDescriptor
import com.androidwebcamera.model.StreamCodec
import com.androidwebcamera.model.StreamConfig
import com.androidwebcamera.model.StreamState
import com.androidwebcamera.model.VideoResolution
import java.util.Locale
import kotlin.math.roundToInt

/**
 * Material 3 configuration card for selecting camera lens, resolution, FPS,
 * codec (H.264 / MJPEG), bitrate/quality, orientation, mirror, and flashlight.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun StreamControlsCard(
    state: StreamState,
    onUpdateConfig: ((StreamConfig) -> StreamConfig) -> Unit,
    modifier: Modifier = Modifier
) {
    val activeCamera = state.activeCamera
    val config = state.config
    val currentRes = activeCamera?.bestResolutionOrNull(config.width, config.height)
        ?: VideoResolution(config.width, config.height)
    val availableFps = activeCamera?.supportedFpsFor(currentRes) ?: listOf(15, 24, 30)

    ElevatedCard(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(24.dp),
        colors = CardDefaults.elevatedCardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow
        )
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            Text(
                text = "Параметры камеры и потока",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold
            )

            // Camera dropdown
            CameraDropdown(
                cameras = state.availableCameras,
                selectedCamera = activeCamera,
                onSelect = { cam ->
                    onUpdateConfig { it.copy(cameraId = cam.id) }
                }
            )

            // Resolution dropdown
            ResolutionDropdown(
                resolutions = activeCamera?.supportedResolutions ?: listOf(currentRes),
                selected = currentRes,
                onSelect = { res ->
                    onUpdateConfig { it.copy(width = res.width, height = res.height) }
                }
            )

            // FPS chips
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    text = "Частота кадров (FPS)",
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    modifier = Modifier.horizontalScroll(rememberScrollState())
                ) {
                    availableFps.forEach { fps ->
                        FilterChip(
                            selected = config.fps == fps,
                            onClick = { onUpdateConfig { it.copy(fps = fps) } },
                            label = { Text("$fps fps") }
                        )
                    }
                }
            }

            // Codec chips
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    text = "Кодек и протокол",
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    modifier = Modifier.horizontalScroll(rememberScrollState())
                ) {
                    StreamCodec.entries.forEach { codec ->
                        val info = state.supportedCodecs.firstOrNull { it.codec == codec }
                        val supported = info?.supported ?: (codec == StreamCodec.MJPEG)
                        if (!supported) return@forEach
                        val labelText = if (codec.isMediaCodec && info?.isHardware == false) {
                            "${codec.displayName} (SW)"
                        } else {
                            codec.displayName
                        }
                        FilterChip(
                            selected = config.codec == codec,
                            onClick = { onUpdateConfig { it.copy(codec = codec) } },
                            label = { Text(labelText) }
                        )
                    }
                }
            }

            // Bitrate / Quality slider (committed on drag finish to avoid encoder churn)
            if (config.codec.isMediaCodec) {
                var sliderBitrate by remember(config.bitrateKbps) {
                    mutableFloatStateOf(config.bitrateKbps.toFloat())
                }
                Column {
                    val displayBitrate = ((sliderBitrate / 500f).roundToInt() * 500).coerceIn(1000, 30000)
                    val mbps = displayBitrate / 1000f
                    Text(
                        text = "Битрейт ${config.codec.displayName}: ${String.format(Locale.US, "%.1f", mbps)} Мбит/с",
                        style = MaterialTheme.typography.labelLarge
                    )
                    Slider(
                        value = sliderBitrate,
                        onValueChange = { sliderBitrate = it },
                        onValueChangeFinished = {
                            val rounded = ((sliderBitrate / 500f).roundToInt() * 500).coerceIn(1000, 30000)
                            onUpdateConfig { it.copy(bitrateKbps = rounded) }
                        },
                        valueRange = 1000f..30000f
                    )
                }
            } else {
                var sliderQuality by remember(config.jpegQuality) {
                    mutableFloatStateOf(config.jpegQuality.toFloat())
                }
                Column {
                    val displayQuality = sliderQuality.roundToInt().coerceIn(45, 95)
                    Text(
                        text = "Качество JPEG: $displayQuality%",
                        style = MaterialTheme.typography.labelLarge
                    )
                    Slider(
                        value = sliderQuality,
                        onValueChange = { sliderQuality = it },
                        onValueChangeFinished = {
                            val rounded = sliderQuality.roundToInt().coerceIn(45, 95)
                            onUpdateConfig { it.copy(jpegQuality = rounded) }
                        },
                        valueRange = 45f..95f
                    )
                }
            }

            // Rotation for clients (Web UI & PC Bridge)
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        imageVector = Icons.AutoMirrored.Rounded.RotateRight,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Spacer(modifier = Modifier.width(6.dp))
                    Text(
                        text = "Поворот картинки для ПК / браузера",
                        style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    modifier = Modifier.horizontalScroll(rememberScrollState())
                ) {
                    listOf(0, 90, 180, 270).forEach { deg ->
                        FilterChip(
                            selected = config.rotationDegrees == deg,
                            onClick = { onUpdateConfig { it.copy(rotationDegrees = deg) } },
                            label = { Text("${deg}°") }
                        )
                    }
                }
            }

            // Mirror & Torch toggles
            Row(
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.weight(1f)
                ) {
                    Icon(Icons.Rounded.Flip, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = "Зеркало",
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.weight(1f)
                    )
                    Switch(
                        checked = config.mirrored,
                        onCheckedChange = { checked ->
                            onUpdateConfig { it.copy(mirrored = checked) }
                        }
                    )
                }

                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.weight(1f)
                ) {
                    Icon(
                        imageVector = if (config.torchEnabled) Icons.Rounded.FlashOn else Icons.Rounded.FlashOff,
                        contentDescription = null
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = "Фонарик",
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.weight(1f)
                    )
                    Switch(
                        checked = config.torchEnabled,
                        enabled = activeCamera?.hasFlash == true,
                        onCheckedChange = { checked ->
                            onUpdateConfig { it.copy(torchEnabled = checked) }
                        }
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CameraDropdown(
    cameras: List<CameraDescriptor>,
    selectedCamera: CameraDescriptor?,
    onSelect: (CameraDescriptor) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }

    ExposedDropdownMenuBox(
        expanded = expanded,
        onExpandedChange = { expanded = !expanded }
    ) {
        OutlinedTextField(
            value = selectedCamera?.name ?: "Камера не найдена",
            onValueChange = {},
            readOnly = true,
            label = { Text("Камера") },
            leadingIcon = { Icon(Icons.Rounded.Cameraswitch, contentDescription = null) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) },
            modifier = Modifier
                .fillMaxWidth()
                .menuAnchor(MenuAnchorType.PrimaryNotEditable)
        )
        ExposedDropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false }
        ) {
            cameras.forEach { cam ->
                DropdownMenuItem(
                    text = { Text("${cam.name} • ${cam.facing.label}") },
                    onClick = {
                        expanded = false
                        onSelect(cam)
                    }
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ResolutionDropdown(
    resolutions: List<VideoResolution>,
    selected: VideoResolution,
    onSelect: (VideoResolution) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }

    ExposedDropdownMenuBox(
        expanded = expanded,
        onExpandedChange = { expanded = !expanded }
    ) {
        OutlinedTextField(
            value = selected.label,
            onValueChange = {},
            readOnly = true,
            label = { Text("Разрешение") },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) },
            modifier = Modifier
                .fillMaxWidth()
                .menuAnchor(MenuAnchorType.PrimaryNotEditable)
        )
        ExposedDropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false }
        ) {
            resolutions.forEach { res ->
                DropdownMenuItem(
                    text = { Text(res.label) },
                    onClick = {
                        expanded = false
                        onSelect(res)
                    }
                )
            }
        }
    }
}
