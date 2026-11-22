package com.androidwebcamera.model

import com.androidwebcamera.camera.CameraStreamController
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class StreamModelsTest {

    @Test
    fun normalizedForClampsUnsupportedResolutionFpsFlashAndCodec() {
        val r720 = VideoResolution(1280, 720)
        val r1080 = VideoResolution(1920, 1080)
        val cam = CameraDescriptor(
            id = "1",
            name = "Фронтальная",
            facing = LensFacing.FRONT,
            focalLengthMm = 2.5f,
            sensorOrientation = 270,
            hasFlash = false,
            supportedResolutions = listOf(r1080, r720),
            supportedFpsByResolution = mapOf(
                r1080 to listOf(15, 24),
                r720 to listOf(15, 30)
            )
        )
        val deviceCodecs = listOf(
            CodecSupportInfo(StreamCodec.H264, supported = true, isHardware = true),
            CodecSupportInfo(StreamCodec.H265, supported = true, isHardware = true),
            CodecSupportInfo(StreamCodec.AV1, supported = false, isHardware = false),
            CodecSupportInfo(StreamCodec.MJPEG, supported = true, isHardware = false)
        )

        val raw = StreamConfig(
            cameraId = "0",
            width = 3840,
            height = 2160,
            fps = 60,
            codec = StreamCodec.AV1, // unsupported on this device
            bitrateKbps = 999_999,
            jpegQuality = 10,
            port = 80,
            rotationDegrees = -90,
            torchEnabled = true
        )

        val normalized = raw.normalizedFor(cam, deviceCodecs)
        assertEquals("1", normalized.cameraId)
        assertEquals(1280, normalized.width)
        assertEquals(720, normalized.height)
        assertEquals(30, normalized.fps)
        assertEquals(StreamCodec.H264, normalized.codec)
        assertEquals(20000, normalized.bitrateKbps)
        assertEquals(40, normalized.jpegQuality)
        assertEquals(1024, normalized.port)
        assertEquals(270, normalized.rotationDegrees)
        assertFalse(normalized.torchEnabled)

        // Supported H.265 must be preserved
        val h265Normalized = raw.copy(codec = StreamCodec.H265).normalizedFor(cam, deviceCodecs)
        assertEquals(StreamCodec.H265, h265Normalized.codec)
    }

    @Test
    fun streamCodecFromApiParsesAllCodecsAndAliases() {
        assertEquals(StreamCodec.H264, StreamCodec.fromApi("h264"))
        assertEquals(StreamCodec.H264, StreamCodec.fromApi("AVC"))
        assertEquals(StreamCodec.H265, StreamCodec.fromApi("h265"))
        assertEquals(StreamCodec.H265, StreamCodec.fromApi("HEVC"))
        assertEquals(StreamCodec.AV1, StreamCodec.fromApi("av1"))
        assertEquals(StreamCodec.AV1, StreamCodec.fromApi("AV01"))
        assertEquals(StreamCodec.MJPEG, StreamCodec.fromApi("MJPEG"))
        assertEquals(StreamCodec.MJPEG, StreamCodec.fromApi("mjpg"))
        assertEquals(StreamCodec.H264, StreamCodec.fromApi("unknown"))
        assertNull(StreamCodec.fromApiOrNull("vp9"))
        assertTrue(VideoResolution(1920, 1080).label.contains("1080p"))
        assertTrue(StreamCodec.H265.isMediaCodec)
        assertTrue(StreamCodec.AV1.isMediaCodec)
        assertFalse(StreamCodec.MJPEG.isMediaCodec)
    }

    @Test
    fun requiresPipelineRestartDistinguishesLiveAndFullReconfigures() {
        val base = StreamConfig(cameraId = "0", width = 1280, height = 720, fps = 30, codec = StreamCodec.H265, bitrateKbps = 4000)

        // Live changes must NOT restart pipeline
        assertFalse(CameraStreamController.requiresPipelineRestart(base, base.copy(bitrateKbps = 8000)))
        assertFalse(CameraStreamController.requiresPipelineRestart(base, base.copy(rotationDegrees = 90)))
        assertFalse(CameraStreamController.requiresPipelineRestart(base, base.copy(mirrored = true)))
        assertFalse(CameraStreamController.requiresPipelineRestart(base, base.copy(torchEnabled = true)))
        assertFalse(CameraStreamController.requiresPipelineRestart(base, base.copy(jpegQuality = 60))) // ignored in H265

        // Structural changes MUST restart pipeline
        assertTrue(CameraStreamController.requiresPipelineRestart(base, base.copy(cameraId = "1")))
        assertTrue(CameraStreamController.requiresPipelineRestart(base, base.copy(width = 1920, height = 1080)))
        assertTrue(CameraStreamController.requiresPipelineRestart(base, base.copy(fps = 60)))
        assertTrue(CameraStreamController.requiresPipelineRestart(base, base.copy(codec = StreamCodec.AV1)))
        assertTrue(
            CameraStreamController.requiresPipelineRestart(
                base.copy(codec = StreamCodec.MJPEG, jpegQuality = 80),
                base.copy(codec = StreamCodec.MJPEG, jpegQuality = 60)
            )
        )
    }

    @Test
    fun previewResolutionCapsAt720pWithMatchingAspectRatio() {
        val r4k = VideoResolution(3840, 2160)
        val r1080 = VideoResolution(1920, 1080)
        val r720 = VideoResolution(1280, 720)
        val rVga = VideoResolution(640, 480)
        val cam = CameraDescriptor(
            id = "0",
            name = "Основная",
            facing = LensFacing.BACK,
            focalLengthMm = 4.7f,
            sensorOrientation = 90,
            hasFlash = true,
            supportedResolutions = listOf(r4k, r1080, r720, rVga),
            supportedFpsByResolution = emptyMap()
        )

        assertEquals(r720, cam.previewResolutionFor(1920, 1080))
        assertEquals(r720, cam.previewResolutionFor(3840, 2160))
        assertEquals(rVga, cam.previewResolutionFor(640, 480))
    }
}
