package com.androidwebcamera.server

import com.androidwebcamera.model.StreamCodec
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class FrameHubTest {

    @Test
    fun subscribeVideoDeliversCachedConfigAndTriggersSyncRequest() {
        val hub = FrameHub()
        var syncRequested = 0
        hub.onRequestSyncFrame = { syncRequested++ }

        val spsPps = byteArrayOf(0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1F)
        hub.publishVideo(
            EncodedVideoFrame(
                codec = StreamCodec.H264,
                data = spsPps,
                ptsUs = 0L,
                isConfig = true,
                isKeyFrame = false
            )
        )

        val sub = hub.subscribeVideo(StreamCodec.H264)
        assertEquals(1, syncRequested)
        assertEquals(1, hub.clientCount.value)

        val cached = sub.channel.tryReceive().getOrNull()
        assertNotNull(cached)
        assertTrue(cached!!.isConfig)
        assertEquals(StreamCodec.H264, cached.codec)
        assertArrayEquals(spsPps, cached.data)

        val keyFrame = byteArrayOf(0, 0, 0, 1, 0x65, 0x88.toByte())
        hub.publishVideo(
            EncodedVideoFrame(
                codec = StreamCodec.H264,
                data = keyFrame,
                ptsUs = 33_333L,
                isConfig = false,
                isKeyFrame = true
            )
        )
        val receivedKey = sub.channel.tryReceive().getOrNull()
        assertNotNull(receivedKey)
        assertTrue(receivedKey!!.isKeyFrame)
        assertArrayEquals(keyFrame, receivedKey.data)

        sub.close()
        assertEquals(0, hub.clientCount.value)
    }

    @Test
    fun generationIsolationDropsStaleEncoderFramesAndSeparatesCodecs() {
        val hub = FrameHub()
        val gen1 = hub.beginNewStream()

        val h264Sub = hub.subscribeVideo(StreamCodec.H264)
        val h264Data = byteArrayOf(0, 0, 0, 1, 0x65, 0x11)
        hub.publishVideo(
            EncodedVideoFrame(StreamCodec.H264, h264Data, ptsUs = 1000L, isConfig = false, isKeyFrame = true, generation = gen1)
        )
        assertNotNull(h264Sub.channel.tryReceive().getOrNull())

        // Switch stream to H.265 -> closes old subscribers and bumps generation
        val gen2 = hub.beginNewStream(closeExistingSubscribers = true)
        assertTrue(gen2 > gen1)
        assertEquals(0, hub.clientCount.value)

        val h265Sub = hub.subscribeVideo(StreamCodec.H265)
        // Stale frame from stopping gen1 encoder must be dropped
        hub.publishVideo(
            EncodedVideoFrame(StreamCodec.H264, h264Data, ptsUs = 2000L, isConfig = false, isKeyFrame = true, generation = gen1)
        )
        assertNull(h265Sub.channel.tryReceive().getOrNull())
        assertNull(hub.latestConfigFrame)

        // Fresh gen2 H.265 frame arrives cleanly
        val h265Cfg = byteArrayOf(0, 0, 0, 1, (32 shl 1).toByte(), 0x01)
        hub.publishVideo(
            EncodedVideoFrame(StreamCodec.H265, h265Cfg, ptsUs = 0L, isConfig = true, isKeyFrame = false, generation = gen2)
        )
        val got = h265Sub.channel.tryReceive().getOrNull()
        assertNotNull(got)
        assertEquals(StreamCodec.H265, got!!.codec)
        assertArrayEquals(h265Cfg, got.data)

        h265Sub.close()
    }

    @Test
    fun subscribeMjpegDeliversLatestAndLiveFrames() {
        val hub = FrameHub()
        val gen = hub.beginNewStream()
        val first = byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 0x01)
        hub.publishJpeg(first, gen)

        val sub = hub.subscribeMjpeg()
        assertEquals(1, hub.clientCount.value)
        assertArrayEquals(first, sub.channel.tryReceive().getOrNull())

        val second = byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 0x02)
        hub.publishJpeg(second, gen)
        assertArrayEquals(second, sub.channel.tryReceive().getOrNull())

        // Stale generation JPEG must be dropped
        hub.publishJpeg(byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 0x99.toByte()), gen - 1)
        assertNull(sub.channel.tryReceive().getOrNull())

        hub.closeAllSubscribers()
        assertEquals(0, hub.clientCount.value)
    }
}
