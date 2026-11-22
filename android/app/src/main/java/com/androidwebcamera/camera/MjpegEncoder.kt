package com.androidwebcamera.camera

import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import com.androidwebcamera.server.FrameHub
import java.io.ByteArrayOutputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Software MJPEG encoder backed by [ImageReader] receiving YUV_420_888 camera frames
 * and compressing each frame into JPEG via [YuvImage].
 *
 * Closing of [imageReader] is serialized on the encoder's [HandlerThread] so native Image
 * buffers are never closed concurrently with [onImageAvailable].
 */
class MjpegEncoder(
    private val width: Int,
    private val height: Int,
    private val jpegQuality: Int,
    private val generation: Long,
    private val frameHub: FrameHub
) : VideoEncoder {

    private val running = AtomicBoolean(false)
    private val thread: HandlerThread
    private val handler: Handler
    private val imageReader: ImageReader

    private val yuvConverter = YuvConverter()
    private val nv21Buffer = ByteArray(width * height * 3 / 2)
    private val jpegStream = ByteArrayOutputStream(maxOf(16 * 1024, width * height / 4))
    private val cropRect = Rect(0, 0, width, height)

    init {
        val reader = ImageReader.newInstance(width, height, ImageFormat.YUV_420_888, 3)
        try {
            val worker = HandlerThread("AwcMjpegEncoder").apply { start() }
            imageReader = reader
            thread = worker
            handler = Handler(worker.looper)
        } catch (t: Throwable) {
            runCatching { reader.close() }
            throw t
        }
    }

    override val inputSurface: Surface
        get() = imageReader.surface

    override fun start() {
        if (!running.compareAndSet(false, true)) return
        imageReader.setOnImageAvailableListener(::onImageAvailable, handler)
    }

    override fun requestSyncFrame() = Unit

    override fun updateBitrate(bitrateKbps: Int) = Unit

    override fun stopAndRelease() {
        running.set(false)
        runCatching { imageReader.setOnImageAvailableListener(null, null) }

        val latch = CountDownLatch(1)
        val posted = if (thread.isAlive) {
            handler.post {
                try {
                    runCatching { imageReader.close() }
                } finally {
                    latch.countDown()
                }
            }
        } else {
            false
        }

        if (posted) {
            if (!latch.await(500, TimeUnit.MILLISECONDS)) {
                runCatching { imageReader.close() }
            }
        } else {
            runCatching { imageReader.close() }
        }
        runCatching { thread.quitSafely() }
    }

    private fun onImageAvailable(reader: ImageReader) {
        if (!running.get()) return
        val image = try {
            reader.acquireLatestImage()
        } catch (t: Throwable) {
            Log.w(TAG, "acquireLatestImage failed", t)
            null
        } ?: return

        try {
            if (!running.get()) return
            val planes = image.planes
            if (planes.size < 3) return
            yuvConverter.yuv420ToNv21(
                width = width,
                height = height,
                yBuffer = planes[0].buffer,
                yRowStride = planes[0].rowStride,
                yPixelStride = planes[0].pixelStride,
                uBuffer = planes[1].buffer,
                uRowStride = planes[1].rowStride,
                uPixelStride = planes[1].pixelStride,
                vBuffer = planes[2].buffer,
                vRowStride = planes[2].rowStride,
                vPixelStride = planes[2].pixelStride,
                out = nv21Buffer
            )
            jpegStream.reset()
            YuvImage(nv21Buffer, ImageFormat.NV21, width, height, null)
                .compressToJpeg(cropRect, jpegQuality, jpegStream)
            frameHub.publishJpeg(jpegStream.toByteArray(), generation)
        } catch (t: Throwable) {
            if (running.get()) {
                Log.w(TAG, "JPEG encode error", t)
            }
        } finally {
            runCatching { image.close() }
        }
    }

    companion object {
        private const val TAG = "MjpegEncoder"
    }
}
