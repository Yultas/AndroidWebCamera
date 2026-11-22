package com.androidwebcamera.server

import android.content.Context
import android.util.Log
import com.androidwebcamera.camera.BitstreamUtils
import com.androidwebcamera.model.StreamCodec
import com.androidwebcamera.model.StreamConfig
import com.androidwebcamera.model.StreamState
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets

/**
 * Embedded HTTP/1.1 + WebSocket server exposing:
 * - `/` offline Web UI
 * - `/api/status` and `/api/config` JSON control endpoints
 * - `/ws/video` universal binary low-latency WebSocket stream (H.264 / H.265 / AV1)
 * - `/ws/h264` legacy binary WebSocket stream (H.264)
 * - `/stream.h264` raw H.264 Annex-B HTTP stream (FFmpeg / OBS / VLC)
 * - `/stream.h265` raw H.265 (HEVC) Annex-B HTTP stream (FFmpeg / OBS / VLC)
 * - `/stream.av1` and `/stream.ivf` IVF-wrapped AV1 HTTP stream (FFmpeg / OBS / VLC)
 * - `/stream.mjpg` multipart MJPEG stream
 * - `/snapshot.jpg` single JPEG frame
 */
class StreamHttpServer(
    private val indexHtmlProvider: () -> ByteArray,
    private val frameHub: FrameHub,
    private val stateProvider: () -> StreamState,
    private val onUpdateConfig: (StreamConfig) -> StreamConfig
) {
    constructor(
        context: Context,
        frameHub: FrameHub,
        stateProvider: () -> StreamState,
        onUpdateConfig: (StreamConfig) -> StreamConfig
    ) : this(
        indexHtmlProvider = { context.assets.open("web/index.html").use { it.readBytes() } },
        frameHub = frameHub,
        stateProvider = stateProvider,
        onUpdateConfig = onUpdateConfig
    )

    private var scope: CoroutineScope? = null
    private var serverSocket: ServerSocket? = null

    @Volatile
    private var boundPort: Int = -1

    val actualPort: Int
        get() = serverSocket?.localPort ?: boundPort

    private val cachedIndexHtml: ByteArray by lazy {
        indexHtmlProvider()
    }

    fun start(port: Int) {
        val currentSock = serverSocket
        if (currentSock != null && !currentSock.isClosed && boundPort == port && scope != null) return
        stop()

        val errHandler = CoroutineExceptionHandler { _, throwable ->
            Log.w(TAG, "Unhandled server coroutine error", throwable)
        }
        val activeScope = CoroutineScope(SupervisorJob() + Dispatchers.IO + errHandler)
        scope = activeScope

        val socket = ServerSocket().apply {
            reuseAddress = true
            bind(InetSocketAddress("0.0.0.0", port))
        }
        serverSocket = socket
        boundPort = port

        activeScope.launch {
            while (isActive && !socket.isClosed) {
                val client = try {
                    socket.accept()
                } catch (_: SocketException) {
                    break
                } catch (t: Throwable) {
                    Log.w(TAG, "accept error", t)
                    continue
                }
                launch { handleClient(activeScope, client) }
            }
        }
    }

    fun stop() {
        runCatching { serverSocket?.close() }
        serverSocket = null
        boundPort = -1
        scope?.cancel()
        scope = null
    }

    private suspend fun handleClient(clientScope: CoroutineScope, socket: Socket) {
        try {
            socket.use { sock ->
                sock.tcpNoDelay = true
                // Prevent idle browser speculative preconnect sockets from hanging forever
                sock.soTimeout = REQUEST_READ_TIMEOUT_MS
                val input = sock.getInputStream()
                val output = sock.getOutputStream()

                val req = readHttpRequest(input) ?: return
                val path = req.path.substringBefore('?')

                if (req.method == "OPTIONS") {
                    writeResponse(output, 204, "No Content", "text/plain", ByteArray(0))
                    return
                }

                when {
                    req.method == "GET" && (path == "/" || path == "/index.html") -> {
                        writeResponse(output, 200, "OK", "text/html; charset=utf-8", cachedIndexHtml)
                    }

                    req.method == "GET" && path == "/api/status" -> {
                        val json = buildStatusJson(stateProvider()).toString().toByteArray(StandardCharsets.UTF_8)
                        writeResponse(output, 200, "OK", "application/json; charset=utf-8", json)
                    }

                    req.method == "POST" && path == "/api/config" -> {
                        handlePostConfig(req.body, output)
                    }

                    req.method == "GET" && path == "/ws/video" -> {
                        sock.soTimeout = 0
                        handleWebSocketVideo(clientScope, req.headers, input, output, legacyH264Format = false)
                    }

                    req.method == "GET" && path == "/ws/h264" -> {
                        sock.soTimeout = 0
                        handleWebSocketVideo(clientScope, req.headers, input, output, legacyH264Format = true)
                    }

                    req.method == "GET" && path == "/stream.h264" -> {
                        sock.soTimeout = 0
                        handleRawAnnexBStream(clientScope, StreamCodec.H264, "video/h264", input, output)
                    }

                    req.method == "GET" && path == "/stream.h265" -> {
                        sock.soTimeout = 0
                        handleRawAnnexBStream(clientScope, StreamCodec.H265, "video/hevc", input, output)
                    }

                    req.method == "GET" && (path == "/stream.av1" || path == "/stream.ivf") -> {
                        sock.soTimeout = 0
                        handleAv1IvfStream(clientScope, input, output)
                    }

                    req.method == "GET" && path == "/stream.mjpg" -> {
                        sock.soTimeout = 0
                        handleMjpegStream(clientScope, input, output)
                    }

                    req.method == "GET" && path == "/snapshot.jpg" -> {
                        handleSnapshot(output)
                    }

                    else -> {
                        val body = """{"error":"Not Found"}""".toByteArray(StandardCharsets.UTF_8)
                        writeResponse(output, 404, "Not Found", "application/json", body)
                    }
                }
            }
        } catch (ce: CancellationException) {
            throw ce
        } catch (_: java.io.IOException) {
            // Expected on timeouts or when remote client disconnects mid-stream (e.g. Broken pipe)
        } catch (t: Throwable) {
            Log.w(TAG, "Client request error", t)
        }
    }

    private fun handlePostConfig(body: ByteArray, output: OutputStream) {
        try {
            val payload = JSONObject(String(body, StandardCharsets.UTF_8).ifBlank { "{}" })
            val currentState = stateProvider()
            val current = currentState.config

            val requestedCodec = if (payload.has("codec") && !payload.isNull("codec")) {
                val rawCodec = payload.optString("codec")
                val parsed = StreamCodec.fromApiOrNull(rawCodec)
                if (parsed == null) {
                    val err = JSONObject()
                        .put("error", "Неизвестный кодек: $rawCodec (доступно: h264, h265, av1, mjpeg)")
                        .toString()
                        .toByteArray(StandardCharsets.UTF_8)
                    writeResponse(output, 400, "Bad Request", "application/json; charset=utf-8", err)
                    return
                }
                if (!currentState.isCodecSupported(parsed)) {
                    val err = JSONObject()
                        .put("error", "Кодек ${parsed.displayName} не поддерживается на этом устройстве")
                        .toString()
                        .toByteArray(StandardCharsets.UTF_8)
                    writeResponse(output, 400, "Bad Request", "application/json; charset=utf-8", err)
                    return
                }
                parsed
            } else {
                current.codec
            }

            val updatedRequest = current.copy(
                cameraId = if (payload.has("cameraId") && !payload.isNull("cameraId")) {
                    payload.optString("cameraId", current.cameraId)
                } else {
                    current.cameraId
                },
                width = payload.optInt("width", current.width),
                height = payload.optInt("height", current.height),
                fps = payload.optInt("fps", current.fps),
                codec = requestedCodec,
                bitrateKbps = payload.optInt("bitrateKbps", current.bitrateKbps),
                jpegQuality = payload.optInt("jpegQuality", current.jpegQuality),
                rotationDegrees = payload.optInt("rotation", current.rotationDegrees),
                mirrored = if (payload.has("mirrored")) payload.optBoolean("mirrored") else current.mirrored,
                torchEnabled = if (payload.has("torchEnabled")) {
                    payload.optBoolean("torchEnabled")
                } else {
                    current.torchEnabled
                }
            )
            onUpdateConfig(updatedRequest)
            val json = buildStatusJson(stateProvider()).toString().toByteArray(StandardCharsets.UTF_8)
            writeResponse(output, 200, "OK", "application/json; charset=utf-8", json)
        } catch (t: Throwable) {
            val err = JSONObject().put("error", t.message ?: "Bad Request")
                .toString()
                .toByteArray(StandardCharsets.UTF_8)
            writeResponse(output, 400, "Bad Request", "application/json; charset=utf-8", err)
        }
    }

    private suspend fun handleWebSocketVideo(
        clientScope: CoroutineScope,
        headers: Map<String, String>,
        input: InputStream,
        output: OutputStream,
        legacyH264Format: Boolean
    ) {
        val wsKey = headers["sec-websocket-key"]
        if (wsKey.isNullOrBlank()) {
            writeResponse(
                output,
                400,
                "Bad Request",
                "text/plain",
                "Missing Sec-WebSocket-Key".toByteArray(StandardCharsets.UTF_8)
            )
            return
        }

        val currentCodec = stateProvider().config.codec
        if (legacyH264Format && currentCodec != StreamCodec.H264) {
            val err = JSONObject()
                .put("error", "Active codec is ${currentCodec.apiValue}. Use /ws/video or /${currentCodec.streamPath}")
                .toString()
                .toByteArray(StandardCharsets.UTF_8)
            writeResponse(output, 409, "Conflict", "application/json; charset=utf-8", err)
            return
        }
        if (!currentCodec.isMediaCodec) {
            val err = JSONObject()
                .put("error", "Active codec is ${currentCodec.apiValue}. Use /${currentCodec.streamPath}")
                .toString()
                .toByteArray(StandardCharsets.UTF_8)
            writeResponse(output, 409, "Conflict", "application/json; charset=utf-8", err)
            return
        }

        val accept = WebSocketUtils.computeAcceptKey(wsKey)
        val handshake = buildString {
            append("HTTP/1.1 101 Switching Protocols\r\n")
            append("Upgrade: websocket\r\n")
            append("Connection: Upgrade\r\n")
            append("Sec-WebSocket-Accept: ").append(accept).append("\r\n")
            append("\r\n")
        }
        output.write(handshake.toByteArray(StandardCharsets.US_ASCII))
        output.flush()

        val writeLock = Any()
        val expectedCodec = if (legacyH264Format) StreamCodec.H264 else currentCodec
        val sub = frameHub.subscribeVideo(expectedCodec)
        val readerJob = clientScope.launch {
            try {
                while (isActive) {
                    val frame = WebSocketUtils.readFrame(input) ?: break
                    when (frame.opcode) {
                        WebSocketUtils.OPCODE_CLOSE -> {
                            synchronized(writeLock) {
                                runCatching {
                                    WebSocketUtils.writeFrame(
                                        output,
                                        WebSocketUtils.OPCODE_CLOSE,
                                        ByteArray(0)
                                    )
                                }
                            }
                            break
                        }

                        WebSocketUtils.OPCODE_PING -> {
                            synchronized(writeLock) {
                                WebSocketUtils.writeFrame(
                                    output,
                                    WebSocketUtils.OPCODE_PONG,
                                    frame.payload
                                )
                            }
                        }
                    }
                }
            } catch (_: Throwable) {
                // Client disconnected
            } finally {
                sub.close()
            }
        }

        try {
            var configSent = false
            var keyFrameSeen = false
            for (frame in sub.channel) {
                if (frame.isConfig) {
                    configSent = true
                } else {
                    if (!keyFrameSeen) {
                        if (!frame.isKeyFrame) continue
                        if (!configSent) {
                            frameHub.latestConfigFrame?.takeIf { it.codec == frame.codec }?.let { cfg ->
                                sendWsVideoPacket(output, writeLock, cfg, legacyH264Format)
                                configSent = true
                            }
                        }
                        keyFrameSeen = true
                    }
                }
                sendWsVideoPacket(output, writeLock, frame, legacyH264Format)
            }
        } catch (_: Throwable) {
            // Broken pipe / closed socket
        } finally {
            readerJob.cancel()
            sub.close()
        }
    }

    private fun sendWsVideoPacket(
        output: OutputStream,
        lock: Any,
        frame: EncodedVideoFrame,
        legacyH264Format: Boolean
    ) {
        val packetType: Byte = when {
            frame.isConfig -> 1
            frame.isKeyFrame -> 2
            else -> 3
        }
        val packet = if (legacyH264Format) {
            ByteBuffer.allocate(9 + frame.data.size).order(ByteOrder.BIG_ENDIAN).apply {
                put(packetType)
                putLong(frame.ptsUs)
                put(frame.data)
            }.array()
        } else {
            ByteBuffer.allocate(10 + frame.data.size).order(ByteOrder.BIG_ENDIAN).apply {
                put(frame.codec.wsCodecId)
                put(packetType)
                putLong(frame.ptsUs)
                put(frame.data)
            }.array()
        }

        synchronized(lock) {
            WebSocketUtils.writeFrame(output, WebSocketUtils.OPCODE_BINARY, packet)
        }
    }

    private suspend fun handleRawAnnexBStream(
        clientScope: CoroutineScope,
        requiredCodec: StreamCodec,
        contentType: String,
        input: InputStream,
        output: OutputStream
    ) {
        val state = stateProvider()
        if (state.config.codec != requiredCodec) {
            val err = JSONObject()
                .put(
                    "error",
                    "Active codec is ${state.config.codec.apiValue}. " +
                        "Switch codec to ${requiredCodec.apiValue} or use /${state.config.codec.streamPath}"
                )
                .toString()
                .toByteArray(StandardCharsets.UTF_8)
            writeResponse(output, 409, "Conflict", "application/json; charset=utf-8", err)
            return
        }

        val header = buildString {
            append("HTTP/1.1 200 OK\r\n")
            append("Content-Type: ").append(contentType).append("\r\n")
            append("Cache-Control: no-store, no-cache, must-revalidate\r\n")
            append("Connection: close\r\n")
            append("Access-Control-Allow-Origin: *\r\n\r\n")
        }
        output.write(header.toByteArray(StandardCharsets.US_ASCII))
        output.flush()

        frameHub.subscribeVideo(requiredCodec).use { sub ->
            val eofJob = monitorClientEof(clientScope, input, sub)
            try {
                var configSent = false
                var keyFrameSeen = false
                for (frame in sub.channel) {
                    if (frame.isConfig) {
                        if (!configSent) {
                            output.write(frame.data)
                            output.flush()
                            configSent = true
                        }
                        continue
                    }
                    if (!keyFrameSeen) {
                        if (!frame.isKeyFrame) continue
                        if (!configSent) {
                            frameHub.latestConfigFrame?.takeIf { it.codec == requiredCodec }?.let { cfg ->
                                output.write(cfg.data)
                                configSent = true
                            }
                        }
                        keyFrameSeen = true
                    }
                    output.write(frame.data)
                    output.flush()
                }
            } finally {
                eofJob.cancel()
            }
        }
    }

    private suspend fun handleAv1IvfStream(
        clientScope: CoroutineScope,
        input: InputStream,
        output: OutputStream
    ) {
        val state = stateProvider()
        if (state.config.codec != StreamCodec.AV1) {
            val err = JSONObject()
                .put(
                    "error",
                    "Active codec is ${state.config.codec.apiValue}. " +
                        "Switch codec to av1 or use /${state.config.codec.streamPath}"
                )
                .toString()
                .toByteArray(StandardCharsets.UTF_8)
            writeResponse(output, 409, "Conflict", "application/json; charset=utf-8", err)
            return
        }

        val header = buildString {
            append("HTTP/1.1 200 OK\r\n")
            append("Content-Type: video/x-ivf\r\n")
            append("Cache-Control: no-store, no-cache, must-revalidate\r\n")
            append("Connection: close\r\n")
            append("Access-Control-Allow-Origin: *\r\n\r\n")
        }
        output.write(header.toByteArray(StandardCharsets.US_ASCII))
        output.write(
            BitstreamUtils.buildIvfFileHeader(
                width = state.config.width,
                height = state.config.height
            )
        )
        output.flush()

        frameHub.subscribeVideo(StreamCodec.AV1).use { sub ->
            val eofJob = monitorClientEof(clientScope, input, sub)
            try {
                var keyFrameSeen = false
                var basePtsUs: Long? = null
                for (frame in sub.channel) {
                    if (frame.isConfig) continue
                    if (!keyFrameSeen) {
                        if (!frame.isKeyFrame) continue
                        keyFrameSeen = true
                        basePtsUs = frame.ptsUs
                    }
                    val frameBytes = if (frame.isKeyFrame && !BitstreamUtils.hasAv1SequenceHeader(frame.data)) {
                        val seqHdr = frameHub.latestConfigFrame?.takeIf { it.codec == StreamCodec.AV1 }?.data
                        if (seqHdr != null) seqHdr + frame.data else frame.data
                    } else {
                        frame.data
                    }
                    val relPts = (frame.ptsUs - (basePtsUs ?: frame.ptsUs)).coerceAtLeast(0L)
                    output.write(BitstreamUtils.buildIvfFrameHeader(frameBytes.size, relPts))
                    output.write(frameBytes)
                    output.flush()
                }
            } finally {
                eofJob.cancel()
            }
        }
    }

    private suspend fun handleMjpegStream(
        clientScope: CoroutineScope,
        input: InputStream,
        output: OutputStream
    ) {
        val state = stateProvider()
        if (state.config.codec != StreamCodec.MJPEG) {
            val err = JSONObject()
                .put(
                    "error",
                    "Active codec is ${state.config.codec.apiValue}. " +
                        "Switch codec to mjpeg or use /${state.config.codec.streamPath}"
                )
                .toString()
                .toByteArray(StandardCharsets.UTF_8)
            writeResponse(output, 409, "Conflict", "application/json; charset=utf-8", err)
            return
        }

        val header = buildString {
            append("HTTP/1.1 200 OK\r\n")
            append("Content-Type: multipart/x-mixed-replace; boundary=awcframe\r\n")
            append("Cache-Control: no-store, no-cache, must-revalidate\r\n")
            append("Connection: close\r\n")
            append("Access-Control-Allow-Origin: *\r\n\r\n")
        }
        output.write(header.toByteArray(StandardCharsets.US_ASCII))
        output.flush()

        frameHub.subscribeMjpeg().use { sub ->
            val eofJob = monitorClientEof(clientScope, input, sub)
            try {
                for (jpeg in sub.channel) {
                    val partHeader = (
                        "--awcframe\r\n" +
                            "Content-Type: image/jpeg\r\n" +
                            "Content-Length: ${jpeg.size}\r\n\r\n"
                        ).toByteArray(StandardCharsets.US_ASCII)
                    output.write(partHeader)
                    output.write(jpeg)
                    output.write("\r\n".toByteArray(StandardCharsets.US_ASCII))
                    output.flush()
                }
            } finally {
                eofJob.cancel()
            }
        }
    }

    private fun monitorClientEof(
        clientScope: CoroutineScope,
        input: InputStream,
        subscription: AutoCloseable
    ) = clientScope.launch {
        try {
            val discard = ByteArray(64)
            while (isActive) {
                if (input.read(discard) < 0) break
            }
        } catch (_: Throwable) {
            // Peer closed or reset connection
        } finally {
            runCatching { subscription.close() }
        }
    }

    private fun handleSnapshot(output: OutputStream) {
        val jpeg = frameHub.latestJpegFrame
        if (jpeg == null) {
            val err = """{"error":"Snapshot unavailable unless codec is MJPEG and frames are arriving"}"""
                .toByteArray(StandardCharsets.UTF_8)
            writeResponse(output, 409, "Conflict", "application/json; charset=utf-8", err)
            return
        }
        writeResponse(output, 200, "OK", "image/jpeg", jpeg)
    }

    private fun buildStatusJson(state: StreamState): JSONObject {
        val cfg = state.config
        val camerasJson = JSONArray()
        for (cam in state.availableCameras) {
            val resArray = JSONArray()
            for (res in cam.supportedResolutions) {
                val fpsArr = JSONArray()
                cam.supportedFpsFor(res).forEach { fpsArr.put(it) }
                resArray.put(
                    JSONObject()
                        .put("width", res.width)
                        .put("height", res.height)
                        .put("fps", fpsArr)
                )
            }
            camerasJson.put(
                JSONObject()
                    .put("id", cam.id)
                    .put("name", cam.name)
                    .put("facing", cam.facing.name.lowercase())
                    .put("hasFlash", cam.hasFlash)
                    .put("sensorOrientation", cam.sensorOrientation)
                    .put("resolutions", resArray)
            )
        }

        val endpointsJson = JSONArray()
        for (ep in state.endpoints) {
            endpointsJson.put(
                JSONObject()
                    .put("label", ep.label)
                    .put("interface", ep.interfaceName)
                    .put("ip", ep.ip)
                    .put("url", ep.url)
                    .put("isUsb", ep.isUsb)
            )
        }

        val codecsJson = JSONArray()
        for (info in state.supportedCodecs) {
            codecsJson.put(
                JSONObject()
                    .put("id", info.codec.apiValue)
                    .put("name", info.codec.displayName)
                    .put("supported", info.supported)
                    .put("hardware", info.isHardware)
                    .put("streamPath", "/" + info.codec.streamPath)
                    .put("encoder", info.encoderName ?: "")
            )
        }

        return JSONObject()
            .put("streaming", state.isStreaming)
            .put("cameraId", cfg.cameraId)
            .put("cameraName", state.activeCamera?.name ?: "Камера ${cfg.cameraId}")
            .put("width", cfg.width)
            .put("height", cfg.height)
            .put("fps", cfg.fps)
            .put("measuredFps", state.measuredFps)
            .put("codec", cfg.codec.apiValue)
            .put("streamPath", "/" + cfg.codec.streamPath)
            .put("bitrateKbps", cfg.bitrateKbps)
            .put("jpegQuality", cfg.jpegQuality)
            .put("port", cfg.port)
            .put("rotation", cfg.rotationDegrees)
            .put("mirrored", cfg.mirrored)
            .put("torchEnabled", cfg.torchEnabled)
            .put("hasFlash", state.activeCamera?.hasFlash == true)
            .put("clients", state.connectedClients)
            .put("supportedCodecs", codecsJson)
            .put("cameras", camerasJson)
            .put("endpoints", endpointsJson)
    }

    private fun writeResponse(
        output: OutputStream,
        code: Int,
        reason: String,
        contentType: String,
        body: ByteArray
    ) {
        val headers = buildString {
            append("HTTP/1.1 ").append(code).append(' ').append(reason).append("\r\n")
            append("Content-Type: ").append(contentType).append("\r\n")
            append("Content-Length: ").append(body.size).append("\r\n")
            append("Cache-Control: no-store\r\n")
            append("Access-Control-Allow-Origin: *\r\n")
            append("Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n")
            append("Access-Control-Allow-Headers: Content-Type\r\n")
            append("Connection: close\r\n\r\n")
        }
        output.write(headers.toByteArray(StandardCharsets.US_ASCII))
        if (body.isNotEmpty()) {
            output.write(body)
        }
        output.flush()
    }

    private data class HttpRequest(
        val method: String,
        val path: String,
        val headers: Map<String, String>,
        val body: ByteArray
    )

    private fun readHttpRequest(input: InputStream): HttpRequest? {
        val headerBuf = ByteArrayOutputStream(1024)
        var state = 0
        while (headerBuf.size() < 16_384) {
            val b = input.read()
            if (b < 0) return null
            headerBuf.write(b)
            state = when {
                state == 0 && b == '\r'.code -> 1
                state == 1 && b == '\n'.code -> 2
                state == 2 && b == '\r'.code -> 3
                state == 3 && b == '\n'.code -> 4
                b == '\n'.code && headerBuf.size() >= 2 -> {
                    val bytes = headerBuf.toByteArray()
                    if (bytes[bytes.size - 2] == '\n'.code.toByte()) 4 else 0
                }

                else -> 0
            }
            if (state == 4) break
        }

        val rawHeaders = String(headerBuf.toByteArray(), StandardCharsets.US_ASCII)
        val lines = rawHeaders.split("\r\n", "\n").filter { it.isNotEmpty() }
        if (lines.isEmpty()) return null

        val parts = lines.first().split(' ')
        if (parts.size < 2) return null
        val method = parts[0].uppercase()
        val path = parts[1]

        val headers = mutableMapOf<String, String>()
        for (line in lines.drop(1)) {
            val idx = line.indexOf(':')
            if (idx > 0) {
                headers[line.substring(0, idx).trim().lowercase()] = line.substring(idx + 1).trim()
            }
        }

        val contentLength = headers["content-length"]?.toIntOrNull()?.coerceIn(0, 128 * 1024) ?: 0
        val body = ByteArray(contentLength)
        var offset = 0
        while (offset < contentLength) {
            val read = input.read(body, offset, contentLength - offset)
            if (read < 0) break
            offset += read
        }

        return HttpRequest(method, path, headers, body)
    }

    companion object {
        private const val TAG = "StreamHttpServer"
        private const val REQUEST_READ_TIMEOUT_MS = 10_000
    }
}
