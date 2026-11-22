package com.androidwebcamera.server

import com.androidwebcamera.model.StreamCodec
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.concurrent.CopyOnWriteArraySet
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

/** Encoded video packet emitted by MediaCodec (H.264 Annex-B, H.265 Annex-B, or AV1 OBU). */
data class EncodedVideoFrame(
    val codec: StreamCodec,
    val data: ByteArray,
    val ptsUs: Long,
    val isConfig: Boolean,
    val isKeyFrame: Boolean,
    val generation: Long = 0L
)

/**
 * Thread-safe fan-out hub distributing encoded H.264 / H.265 / AV1 and MJPEG frames
 * from the camera pipeline to all connected HTTP and WebSocket clients.
 *
 * Uses a monotonic [currentGeneration] so frames drained from a stopping encoder
 * never leak into a newly started stream or corrupt cross-codec clients.
 */
class FrameHub {

    class VideoSubscription internal constructor(
        val channel: Channel<EncodedVideoFrame>,
        private val onClose: () -> Unit
    ) : AutoCloseable {
        override fun close() = onClose()
    }

    class MjpegSubscription internal constructor(
        val channel: Channel<ByteArray>,
        private val onClose: () -> Unit
    ) : AutoCloseable {
        override fun close() = onClose()
    }

    private data class VideoSubscriberEntry(
        val expectedCodec: StreamCodec?,
        val channel: Channel<EncodedVideoFrame>
    )

    private val videoSubscribers = CopyOnWriteArraySet<VideoSubscriberEntry>()
    private val mjpegSubscribers = CopyOnWriteArraySet<Channel<ByteArray>>()

    private val generationCounter = AtomicLong(1L)
    val currentGeneration: Long
        get() = generationCounter.get()

    @Volatile
    var latestConfigFrame: EncodedVideoFrame? = null
        private set

    @Volatile
    var latestJpegFrame: ByteArray? = null
        private set

    @Volatile
    var onRequestSyncFrame: (() -> Unit)? = null

    private val _clientCount = MutableStateFlow(0)
    val clientCount: StateFlow<Int> = _clientCount.asStateFlow()

    private val _measuredFps = MutableStateFlow(0)
    val measuredFps: StateFlow<Int> = _measuredFps.asStateFlow()

    private val fpsWindowStartNs = AtomicLong(0L)
    private val fpsFrameCounter = AtomicInteger(0)

    /**
     * Advances the stream generation, clears cached frames, and optionally closes existing
     * subscribers so clients reconnect cleanly on format/codec/camera changes.
     */
    fun beginNewStream(closeExistingSubscribers: Boolean = true): Long {
        val nextGen = generationCounter.incrementAndGet()
        clearCachedFrames()
        if (closeExistingSubscribers) {
            closeAllSubscribers()
        }
        return nextGen
    }

    fun subscribeVideo(expectedCodec: StreamCodec? = null): VideoSubscription {
        val channel = Channel<EncodedVideoFrame>(
            capacity = 24,
            onBufferOverflow = BufferOverflow.DROP_OLDEST
        )
        val entry = VideoSubscriberEntry(expectedCodec, channel)
        videoSubscribers.add(entry)
        latestConfigFrame?.let { cfg ->
            if (expectedCodec == null || cfg.codec == expectedCodec) {
                channel.trySend(cfg)
            }
        }
        updateClientCount()
        onRequestSyncFrame?.invoke()
        return VideoSubscription(channel) {
            if (videoSubscribers.remove(entry)) {
                channel.close()
                updateClientCount()
            }
        }
    }

    fun subscribeMjpeg(): MjpegSubscription {
        val channel = Channel<ByteArray>(
            capacity = 2,
            onBufferOverflow = BufferOverflow.DROP_OLDEST
        )
        mjpegSubscribers.add(channel)
        latestJpegFrame?.let { channel.trySend(it) }
        updateClientCount()
        return MjpegSubscription(channel) {
            if (mjpegSubscribers.remove(channel)) {
                channel.close()
                updateClientCount()
            }
        }
    }

    fun publishVideo(frame: EncodedVideoFrame) {
        if (frame.generation != 0L && frame.generation != generationCounter.get()) {
            return
        }
        if (frame.isConfig) {
            latestConfigFrame = frame
        } else {
            recordFrameRateSample()
        }
        for (entry in videoSubscribers) {
            if (entry.expectedCodec == null || entry.expectedCodec == frame.codec) {
                entry.channel.trySend(frame)
            }
        }
    }

    fun publishJpeg(jpegBytes: ByteArray, generation: Long = 0L) {
        if (generation != 0L && generation != generationCounter.get()) {
            return
        }
        latestJpegFrame = jpegBytes
        recordFrameRateSample()
        for (subscriber in mjpegSubscribers) {
            subscriber.trySend(jpegBytes)
        }
    }

    fun clearCachedFrames() {
        latestConfigFrame = null
        latestJpegFrame = null
        fpsWindowStartNs.set(0L)
        fpsFrameCounter.set(0)
        _measuredFps.value = 0
    }

    /** Closes all active client stream channels so consumers cleanly reconnect after a format change. */
    fun closeAllSubscribers() {
        val video = videoSubscribers.toList()
        videoSubscribers.clear()
        video.forEach { it.channel.close() }

        val mjpeg = mjpegSubscribers.toList()
        mjpegSubscribers.clear()
        mjpeg.forEach { it.close() }

        updateClientCount()
    }

    private fun updateClientCount() {
        _clientCount.value = videoSubscribers.size + mjpegSubscribers.size
    }

    private fun recordFrameRateSample() {
        val now = System.nanoTime()
        val prev = fpsWindowStartNs.get()
        if (prev == 0L) {
            fpsWindowStartNs.compareAndSet(0L, now)
            fpsFrameCounter.set(1)
            return
        }
        val count = fpsFrameCounter.incrementAndGet()
        val elapsedNs = now - prev
        if (elapsedNs >= 1_000_000_000L && fpsWindowStartNs.compareAndSet(prev, now)) {
            val fps = ((count * 1_000_000_000.0) / elapsedNs.toDouble() + 0.5).toInt()
            fpsFrameCounter.set(0)
            _measuredFps.value = fps
        }
    }
}
