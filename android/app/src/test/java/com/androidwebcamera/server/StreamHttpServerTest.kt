package com.androidwebcamera.server

import com.androidwebcamera.model.CameraDescriptor
import com.androidwebcamera.model.CodecSupportInfo
import com.androidwebcamera.model.LensFacing
import com.androidwebcamera.model.StreamCodec
import com.androidwebcamera.model.StreamConfig
import com.androidwebcamera.model.StreamState
import com.androidwebcamera.model.VideoResolution
import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.Socket
import java.net.URL
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import java.util.concurrent.atomic.AtomicReference

class StreamHttpServerTest {

    private fun allCodecsSupported(): List<CodecSupportInfo> = listOf(
        CodecSupportInfo(StreamCodec.H264, supported = true, isHardware = true, encoderName = "hw.avc"),
        CodecSupportInfo(StreamCodec.H265, supported = true, isHardware = true, encoderName = "hw.hevc"),
        CodecSupportInfo(StreamCodec.AV1, supported = true, isHardware = true, encoderName = "hw.av1"),
        CodecSupportInfo(StreamCodec.MJPEG, supported = true, isHardware = false, encoderName = "jpeg")
    )

    @Test
    fun servesHtmlStatusConfigValidationAndRestartCleanly() {
        val hub = FrameHub()
        val r720 = VideoResolution(1280, 720)
        val cam = CameraDescriptor(
            id = "0",
            name = "Основная",
            facing = LensFacing.BACK,
            focalLengthMm = 4.7f,
            sensorOrientation = 90,
            hasFlash = true,
            supportedResolutions = listOf(r720),
            supportedFpsByResolution = mapOf(r720 to listOf(30))
        )
        val deviceCodecs = listOf(
            CodecSupportInfo(StreamCodec.H264, supported = true, isHardware = true, encoderName = "hw.avc"),
            CodecSupportInfo(StreamCodec.H265, supported = true, isHardware = true, encoderName = "hw.hevc"),
            CodecSupportInfo(StreamCodec.AV1, supported = false, isHardware = false, encoderName = null),
            CodecSupportInfo(StreamCodec.MJPEG, supported = true, isHardware = false, encoderName = "jpeg")
        )
        val stateRef = AtomicReference(
            StreamState(
                isStreaming = true,
                config = StreamConfig(cameraId = "0", width = 1280, height = 720, fps = 30),
                availableCameras = listOf(cam),
                activeCamera = cam,
                supportedCodecs = deviceCodecs
            )
        )

        val server = StreamHttpServer(
            indexHtmlProvider = { "<html><body>AWC</body></html>".toByteArray(StandardCharsets.UTF_8) },
            frameHub = hub,
            stateProvider = { stateRef.get() },
            onUpdateConfig = { next ->
                stateRef.updateAndGet { it.copy(config = next) }.config
            }
        )

        try {
            server.start(0)
            val port = server.actualPort
            assertTrue(port > 0)

            // 1. GET /
            val html = URL("http://127.0.0.1:$port/").readText()
            assertTrue(html.contains("AWC"))

            // 2. GET /api/status exposes supportedCodecs
            val statusJson = JSONObject(URL("http://127.0.0.1:$port/api/status").readText())
            assertEquals(true, statusJson.getBoolean("streaming"))
            assertEquals("h264", statusJson.getString("codec"))
            assertEquals("/stream.h264", statusJson.getString("streamPath"))
            assertEquals(4, statusJson.getJSONArray("supportedCodecs").length())

            // 3. POST /api/config switches to H.265 cleanly
            val connH265 = postJson(port, """{"codec":"hevc","rotation":90,"mirrored":true,"torchEnabled":true}""")
            assertEquals(200, connH265.responseCode)
            val updated = JSONObject(connH265.inputStream.bufferedReader().readText())
            assertEquals("h265", updated.getString("codec"))
            assertEquals("/stream.h265", updated.getString("streamPath"))
            assertEquals(90, updated.getInt("rotation"))
            assertEquals(true, updated.getBoolean("mirrored"))
            assertEquals(true, updated.getBoolean("torchEnabled"))

            // 4. POST /api/config rejects unsupported AV1 on this device with 400
            val connAv1 = postJson(port, """{"codec":"av1"}""")
            assertEquals(400, connAv1.responseCode)

            // 5. POST /api/config rejects unknown codec with 400
            val connBad = postJson(port, """{"codec":"vp9"}""")
            assertEquals(400, connBad.responseCode)

            // 6. POST /api/config rejects malformed JSON with 400
            val connBroken = postJson(port, """{not valid json""")
            assertEquals(400, connBroken.responseCode)

            // 7. Verify stop() -> start(0) cleanly accepts connections on a new scope
            server.stop()
            server.start(0)
            val port2 = server.actualPort
            val status2 = JSONObject(URL("http://127.0.0.1:$port2/api/status").readText())
            assertEquals("h265", status2.getString("codec"))
        } finally {
            server.stop()
        }
    }

    @Test
    fun streamsH264OverLegacyWsAndH265OverUniversalWs() {
        val hub = FrameHub()
        val stateRef = AtomicReference(
            StreamState(
                isStreaming = true,
                config = StreamConfig(cameraId = "0", codec = StreamCodec.H264),
                supportedCodecs = allCodecsSupported()
            )
        )

        val server = StreamHttpServer(
            indexHtmlProvider = { ByteArray(0) },
            frameHub = hub,
            stateProvider = { stateRef.get() },
            onUpdateConfig = { it }
        )

        val h264Config = byteArrayOf(0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1F, 0, 0, 0, 1, 0x68, 0xCE.toByte())
        val h264Key = byteArrayOf(0, 0, 0, 1, 0x65, 0x88.toByte(), 0x84.toByte())

        val h265Config = byteArrayOf(
            0, 0, 0, 1, (32 shl 1).toByte(), 0x01,
            0, 0, 0, 1, (33 shl 1).toByte(), 0x01,
            0, 0, 0, 1, (34 shl 1).toByte(), 0x01
        )
        val h265Key = byteArrayOf(0, 0, 0, 1, (19 shl 1).toByte(), 0x01, 0xAA.toByte())

        try {
            server.start(0)
            val port = server.actualPort

            // 1. Legacy /ws/h264 (9-byte header)
            hub.publishVideo(EncodedVideoFrame(StreamCodec.H264, h264Config, 0L, isConfig = true, isKeyFrame = false))
            openWebSocket(port, "/ws/h264").use { sock ->
                val input = sock.getInputStream()
                val wsCfg = WebSocketUtils.readFrame(input)!!
                assertEquals(WebSocketUtils.OPCODE_BINARY, wsCfg.opcode)
                assertEquals(1.toByte(), wsCfg.payload[0])
                assertArrayEquals(h264Config, wsCfg.payload.copyOfRange(9, wsCfg.payload.size))

                hub.publishVideo(EncodedVideoFrame(StreamCodec.H264, h264Key, 42_000L, isConfig = false, isKeyFrame = true))
                val wsKey = WebSocketUtils.readFrame(input)!!
                assertEquals(2.toByte(), wsKey.payload[0])
                val pts = ByteBuffer.wrap(wsKey.payload, 1, 8).order(ByteOrder.BIG_ENDIAN).long
                assertEquals(42_000L, pts)
                assertArrayEquals(h264Key, wsKey.payload.copyOfRange(9, wsKey.payload.size))
            }

            // 2. Switch to H.265 -> /ws/video (10-byte header with codecId = 2)
            val gen2 = hub.beginNewStream()
            stateRef.updateAndGet { it.copy(config = it.config.copy(codec = StreamCodec.H265)) }
            hub.publishVideo(EncodedVideoFrame(StreamCodec.H265, h265Config, 0L, isConfig = true, isKeyFrame = false, generation = gen2))

            openWebSocket(port, "/ws/video").use { sock ->
                val input = sock.getInputStream()
                val wsCfg = WebSocketUtils.readFrame(input)!!
                assertEquals(StreamCodec.H265.wsCodecId, wsCfg.payload[0])
                assertEquals(1.toByte(), wsCfg.payload[1])
                assertArrayEquals(h265Config, wsCfg.payload.copyOfRange(10, wsCfg.payload.size))

                hub.publishVideo(EncodedVideoFrame(StreamCodec.H265, h265Key, 99_000L, isConfig = false, isKeyFrame = true, generation = gen2))
                val wsKey = WebSocketUtils.readFrame(input)!!
                assertEquals(StreamCodec.H265.wsCodecId, wsKey.payload[0])
                assertEquals(2.toByte(), wsKey.payload[1])
                val pts = ByteBuffer.wrap(wsKey.payload, 2, 8).order(ByteOrder.BIG_ENDIAN).long
                assertEquals(99_000L, pts)
                assertArrayEquals(h265Key, wsKey.payload.copyOfRange(10, wsKey.payload.size))
            }

            // 3. Mismatched /stream.h264 while active codec is H.265 returns 409 Conflict
            val conflictConn = URL("http://127.0.0.1:$port/stream.h264").openConnection() as HttpURLConnection
            assertEquals(409, conflictConn.responseCode)

            // 4. Abrupt client disconnect on /stream.h265 does not crash server
            Socket("127.0.0.1", port).use { rawSock ->
                rawSock.getOutputStream().write("GET /stream.h265 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n".toByteArray())
                rawSock.getOutputStream().flush()
                Thread.sleep(60)
            }
            for (i in 0..3) {
                hub.publishVideo(
                    EncodedVideoFrame(StreamCodec.H265, h265Key, 100_000L + i * 33_000L, isConfig = false, isKeyFrame = true, generation = gen2)
                )
                Thread.sleep(20)
            }
            val statusAfter = JSONObject(URL("http://127.0.0.1:$port/api/status").readText())
            assertEquals(true, statusAfter.getBoolean("streaming"))
        } finally {
            server.stop()
        }
    }

    @Test
    fun streamsAv1AsValidIvfAndMjpegWithSnapshot() {
        val hub = FrameHub()
        val stateRef = AtomicReference(
            StreamState(
                isStreaming = true,
                config = StreamConfig(cameraId = "0", width = 640, height = 480, codec = StreamCodec.AV1),
                supportedCodecs = allCodecsSupported()
            )
        )
        val server = StreamHttpServer(
            indexHtmlProvider = { ByteArray(0) },
            frameHub = hub,
            stateProvider = { stateRef.get() },
            onUpdateConfig = { it }
        )

        val av1SeqObu = byteArrayOf(0x0A, 0x04, 0x00, 0x00, 0x00, 0x42)
        val av1FrameObu = byteArrayOf(0x32, 0x03, 0x10, 0x20, 0x30)
        val av1Key = av1SeqObu + av1FrameObu

        try {
            server.start(0)
            val port = server.actualPort

            // 1. /stream.av1 wraps frames in IVF (32-byte DKIF/AV01 header + 12-byte frame header + OBU)
            Socket("127.0.0.1", port).use { sock ->
                sock.soTimeout = 3000
                sock.getOutputStream().write("GET /stream.av1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n".toByteArray())
                sock.getOutputStream().flush()

                val input = sock.getInputStream()
                val header = readHttpHeaders(input)
                assertTrue(header.contains("200 OK"))
                assertTrue(header.contains("video/x-ivf"))

                val ivfFileHdr = readExact(input, 32)
                assertEquals("DKIF", String(ivfFileHdr, 0, 4))
                assertEquals("AV01", String(ivfFileHdr, 8, 4))

                // Publish keyframe -> read 12-byte IVF frame header + frame bytes
                Thread {
                    Thread.sleep(30)
                    hub.publishVideo(
                        EncodedVideoFrame(StreamCodec.AV1, av1Key, 500_000L, isConfig = false, isKeyFrame = true)
                    )
                }.start()

                val frameHdr = readExact(input, 12)
                val fb = ByteBuffer.wrap(frameHdr).order(ByteOrder.LITTLE_ENDIAN)
                assertEquals(av1Key.size, fb.getInt(0))
                assertEquals(0L, fb.getLong(4)) // relative PTS starts at 0
                val payload = readExact(input, av1Key.size)
                assertArrayEquals(av1Key, payload)
            }

            // 2. Snapshot returns 409 before MJPEG frames arrive
            val snap409 = URL("http://127.0.0.1:$port/snapshot.jpg").openConnection() as HttpURLConnection
            assertEquals(409, snap409.responseCode)

            // 3. Switch to MJPEG -> /snapshot.jpg & /stream.mjpg succeed
            val genMjpeg = hub.beginNewStream()
            stateRef.updateAndGet { it.copy(config = it.config.copy(codec = StreamCodec.MJPEG)) }
            val fakeJpeg = byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 0x01, 0x02, 0xFF.toByte(), 0xD9.toByte())
            hub.publishJpeg(fakeJpeg, genMjpeg)

            val snapConn = URL("http://127.0.0.1:$port/snapshot.jpg").openConnection() as HttpURLConnection
            assertEquals(200, snapConn.responseCode)
            assertArrayEquals(fakeJpeg, snapConn.inputStream.readBytes())
        } finally {
            server.stop()
        }
    }

    private fun postJson(port: Int, jsonBody: String): HttpURLConnection =
        (URL("http://127.0.0.1:$port/api/config").openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"
            doOutput = true
            setRequestProperty("Content-Type", "application/json")
            outputStream.use { it.write(jsonBody.toByteArray(StandardCharsets.UTF_8)) }
        }

    private fun openWebSocket(port: Int, path: String): Socket {
        val sock = Socket("127.0.0.1", port).apply { soTimeout = 3000 }
        val req = (
            "GET $path HTTP/1.1\r\n" +
                "Host: 127.0.0.1:$port\r\n" +
                "Upgrade: websocket\r\n" +
                "Connection: Upgrade\r\n" +
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
                "Sec-WebSocket-Version: 13\r\n\r\n"
            ).toByteArray(StandardCharsets.US_ASCII)
        sock.getOutputStream().write(req)
        sock.getOutputStream().flush()
        val respHeader = readHttpHeaders(sock.getInputStream())
        assertTrue("Expected 101 Switching Protocols, got: $respHeader", respHeader.contains("101 Switching Protocols"))
        assertTrue(respHeader.contains("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="))
        return sock
    }

    private fun readHttpHeaders(input: InputStream): String {
        val buf = ByteArrayOutputStream()
        var prev4 = 0
        while (true) {
            val b = input.read()
            assertTrue("Unexpected EOF in HTTP headers", b >= 0)
            buf.write(b)
            prev4 = ((prev4 shl 8) or (b and 0xFF))
            if (prev4 == 0x0D0A0D0A) break
        }
        return String(buf.toByteArray(), StandardCharsets.US_ASCII)
    }

    private fun readExact(input: InputStream, count: Int): ByteArray {
        val out = ByteArray(count)
        var off = 0
        while (off < count) {
            val n = input.read(out, off, count - off)
            assertTrue("Unexpected EOF reading $count bytes (got $off)", n >= 0)
            off += n
        }
        return out
    }
}
