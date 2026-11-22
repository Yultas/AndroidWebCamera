package com.androidwebcamera.ui.components

import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.TextureView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Lock
import androidx.compose.material.icons.rounded.Videocam
import androidx.compose.material.icons.rounded.VisibilityOff
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.androidwebcamera.model.LensFacing
import com.androidwebcamera.model.StreamState
import com.androidwebcamera.model.VideoResolution

/**
 * Displays the live camera preview via [TextureView] when active and visible.
 * Automatically detaches the preview [Surface] on lifecycle `ON_STOP` (including screen lock)
 * and defers releasing the Surface / SurfaceTexture until Camera2 confirms the capture session
 * has stopped targeting it.
 */
@Composable
fun CameraPreviewCard(
    state: StreamState,
    showPreview: Boolean,
    onAttachSurface: (Surface) -> Unit,
    onDetachSurface: (Surface, (() -> Unit)?) -> Unit,
    modifier: Modifier = Modifier
) {
    val lifecycleOwner = LocalLifecycleOwner.current
    var textureViewRef by remember { mutableStateOf<TextureView?>(null) }
    var attachedSurface by remember { mutableStateOf<Surface?>(null) }

    val latestAttach by rememberUpdatedState(onAttachSurface)
    val latestDetach by rememberUpdatedState(onDetachSurface)

    fun attachFromTexture(view: TextureView) {
        val tex = view.surfaceTexture ?: return
        if (attachedSurface?.isValid == true) return
        val surf = Surface(tex)
        attachedSurface = surf
        latestAttach(surf)
    }

    fun detachCurrentSurface(releaseTexture: SurfaceTexture? = null) {
        val surf = attachedSurface
        attachedSurface = null
        if (surf != null) {
            latestDetach(surf) {
                runCatching { surf.release() }
                releaseTexture?.let { runCatching { it.release() } }
            }
        } else if (releaseTexture != null) {
            runCatching { releaseTexture.release() }
        }
    }

    DisposableEffect(lifecycleOwner, showPreview, state.isStreaming) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_STOP -> detachCurrentSurface()
                Lifecycle.Event.ON_START -> {
                    val tv = textureViewRef
                    if (showPreview && state.isStreaming && tv != null && tv.isAvailable) {
                        attachFromTexture(tv)
                    }
                }

                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            detachCurrentSurface()
        }
    }

    ElevatedCard(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(24.dp),
        colors = CardDefaults.elevatedCardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow
        )
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(16f / 9f)
                    .clip(RoundedCornerShape(16.dp))
                    .background(MaterialTheme.colorScheme.surfaceContainerHighest),
                contentAlignment = Alignment.Center
            ) {
                if (state.isStreaming && showPreview) {
                    val sensorOrientation by rememberUpdatedState(state.activeCamera?.sensorOrientation ?: 90)
                    val isFront by rememberUpdatedState(state.activeCamera?.facing == LensFacing.FRONT)
                    val videoW by rememberUpdatedState(state.config.width)
                    val videoH by rememberUpdatedState(state.config.height)
                    val previewRes by rememberUpdatedState(
                        remember(state.activeCamera?.id, state.config.width, state.config.height) {
                            state.activeCamera?.previewResolutionFor(state.config.width, state.config.height)
                                ?: VideoResolution(
                                    minOf(state.config.width, 1280),
                                    minOf(state.config.height, 720)
                                )
                        }
                    )
                    val mirrored by rememberUpdatedState(state.config.mirrored)
                    var lastBufferSize by remember { mutableStateOf<Pair<Int, Int>?>(null) }

                    AndroidView(
                        modifier = Modifier.fillMaxSize(),
                        factory = { ctx ->
                            TextureView(ctx).apply {
                                textureViewRef = this
                                surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                                    override fun onSurfaceTextureAvailable(
                                        surfaceTexture: SurfaceTexture,
                                        width: Int,
                                        height: Int
                                    ) {
                                        val res = previewRes
                                        surfaceTexture.setDefaultBufferSize(res.width, res.height)
                                        lastBufferSize = res.width to res.height
                                        configurePreviewTransform(
                                            view = this@apply,
                                            viewWidth = width,
                                            viewHeight = height,
                                            videoWidth = videoW,
                                            videoHeight = videoH,
                                            sensorOrientation = sensorOrientation,
                                            isFrontCamera = isFront,
                                            mirrored = mirrored
                                        )
                                        attachFromTexture(this@apply)
                                    }

                                    override fun onSurfaceTextureSizeChanged(
                                        surfaceTexture: SurfaceTexture,
                                        width: Int,
                                        height: Int
                                    ) {
                                        configurePreviewTransform(
                                            view = this@apply,
                                            viewWidth = width,
                                            viewHeight = height,
                                            videoWidth = videoW,
                                            videoHeight = videoH,
                                            sensorOrientation = sensorOrientation,
                                            isFrontCamera = isFront,
                                            mirrored = mirrored
                                        )
                                    }

                                    override fun onSurfaceTextureDestroyed(surfaceTexture: SurfaceTexture): Boolean {
                                        lastBufferSize = null
                                        // Return false so we release SurfaceTexture only after Camera2
                                        // has finished draining the preview Surface.
                                        detachCurrentSurface(releaseTexture = surfaceTexture)
                                        return false
                                    }

                                    override fun onSurfaceTextureUpdated(surfaceTexture: SurfaceTexture) = Unit
                                }
                            }
                        },
                        update = { tv ->
                            textureViewRef = tv
                            val desiredSize = previewRes.width to previewRes.height
                            if (tv.isAvailable && lastBufferSize != desiredSize) {
                                tv.surfaceTexture?.setDefaultBufferSize(previewRes.width, previewRes.height)
                                lastBufferSize = desiredSize
                            }
                            if (tv.width > 0 && tv.height > 0) {
                                configurePreviewTransform(
                                    view = tv,
                                    viewWidth = tv.width,
                                    viewHeight = tv.height,
                                    videoWidth = videoW,
                                    videoHeight = videoH,
                                    sensorOrientation = sensorOrientation,
                                    isFrontCamera = isFront,
                                    mirrored = mirrored
                                )
                            }
                        }
                    )
                } else {
                    Column(
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                        modifier = Modifier.padding(16.dp)
                    ) {
                        Icon(
                            imageVector = if (state.isStreaming) Icons.Rounded.VisibilityOff else Icons.Rounded.Videocam,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.size(36.dp)
                        )
                        Text(
                            text = if (state.isStreaming) {
                                "Превью скрыто — трансляция продолжается в фоне"
                            } else {
                                "Нажмите «Начать трансляцию» внизу для запуска сервера"
                            },
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            textAlign = TextAlign.Center
                        )
                    }
                }

                if (state.isStreaming) {
                    Surface(
                        color = MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.9f),
                        shape = RoundedCornerShape(12.dp),
                        modifier = Modifier
                            .align(Alignment.TopStart)
                            .padding(10.dp)
                    ) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp)
                        ) {
                            Text(
                                text = "${state.config.width}×${state.config.height} • ${state.measuredFps}/${state.config.fps} fps • ${state.config.codec.apiValue.uppercase()}",
                                style = MaterialTheme.typography.labelMedium,
                                fontWeight = FontWeight.SemiBold,
                                color = MaterialTheme.colorScheme.onPrimaryContainer
                            )
                        }
                    }
                }
            }

            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 10.dp, start = 4.dp, end = 4.dp)
            ) {
                Icon(
                    imageVector = Icons.Rounded.Lock,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.size(16.dp)
                )
                Spacer(modifier = Modifier.width(6.dp))
                Text(
                    text = "Экран можно заблокировать кнопкой питания — поток продолжит идти в фоне.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

private fun configurePreviewTransform(
    view: TextureView,
    viewWidth: Int,
    viewHeight: Int,
    videoWidth: Int,
    videoHeight: Int,
    sensorOrientation: Int,
    isFrontCamera: Boolean,
    mirrored: Boolean
) {
    if (viewWidth == 0 || viewHeight == 0 || videoWidth == 0 || videoHeight == 0) return

    val matrix = Matrix()
    val cx = viewWidth / 2f
    val cy = viewHeight / 2f

    val displayDegrees = when (view.display?.rotation) {
        Surface.ROTATION_90 -> 90
        Surface.ROTATION_180 -> 180
        Surface.ROTATION_270 -> 270
        else -> 0
    }

    val relativeRotation = if (isFrontCamera) {
        (sensorOrientation + displayDegrees) % 360
    } else {
        (sensorOrientation - displayDegrees + 360) % 360
    }

    val viewRatio = viewWidth.toFloat() / viewHeight.toFloat()
    val videoRatio = videoWidth.toFloat() / videoHeight.toFloat()

    if (relativeRotation == 90 || relativeRotation == 270) {
        matrix.postRotate(relativeRotation.toFloat(), cx, cy)
        val rotatedRatio = videoHeight.toFloat() / videoWidth.toFloat()
        val targetH = if (rotatedRatio > viewRatio) viewWidth.toFloat() / rotatedRatio else viewHeight.toFloat()
        val targetW = targetH * rotatedRatio
        val sx = targetW / viewHeight.toFloat()
        val sy = targetH / viewWidth.toFloat()
        matrix.postScale(sx, sy, cx, cy)
    } else {
        if (relativeRotation != 0) {
            matrix.postRotate(relativeRotation.toFloat(), cx, cy)
        }
        if (videoRatio > viewRatio) {
            matrix.postScale(1f, viewRatio / videoRatio, cx, cy)
        } else {
            matrix.postScale(videoRatio / viewRatio, 1f, cx, cy)
        }
    }

    val effectiveMirror = if (isFrontCamera) !mirrored else mirrored
    if (effectiveMirror) {
        matrix.postScale(-1f, 1f, cx, cy)
    }
    view.setTransform(matrix)
}
