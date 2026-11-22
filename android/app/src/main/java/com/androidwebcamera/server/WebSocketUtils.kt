package com.androidwebcamera.server

import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream
import java.nio.charset.StandardCharsets
import java.security.MessageDigest
import java.util.Base64

/** Minimal RFC 6455 WebSocket framing and handshake helpers. */
object WebSocketUtils {

    private const val WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    const val OPCODE_CONTINUATION = 0x0
    const val OPCODE_TEXT = 0x1
    const val OPCODE_BINARY = 0x2
    const val OPCODE_CLOSE = 0x8
    const val OPCODE_PING = 0x9
    const val OPCODE_PONG = 0xA

    data class WsFrame(
        val fin: Boolean,
        val opcode: Int,
        val payload: ByteArray
    )

    /** Computes the RFC 6455 Sec-WebSocket-Accept header value for [secWebSocketKey]. */
    fun computeAcceptKey(secWebSocketKey: String): String {
        val digest = MessageDigest.getInstance("SHA-1")
        val combined = (secWebSocketKey.trim() + WEBSOCKET_GUID).toByteArray(StandardCharsets.US_ASCII)
        return Base64.getEncoder().encodeToString(digest.digest(combined))
    }

    /** Writes a single unmasked server-to-client WebSocket frame to [out]. */
    fun writeFrame(out: OutputStream, opcode: Int, payload: ByteArray) {
        val len = payload.size
        val header = when {
            len <= 125 -> byteArrayOf(
                (0x80 or (opcode and 0x0F)).toByte(),
                len.toByte()
            )

            len <= 65535 -> byteArrayOf(
                (0x80 or (opcode and 0x0F)).toByte(),
                126.toByte(),
                ((len ushr 8) and 0xFF).toByte(),
                (len and 0xFF).toByte()
            )

            else -> {
                val longLen = len.toLong()
                byteArrayOf(
                    (0x80 or (opcode and 0x0F)).toByte(),
                    127.toByte(),
                    ((longLen ushr 56) and 0xFF).toByte(),
                    ((longLen ushr 48) and 0xFF).toByte(),
                    ((longLen ushr 40) and 0xFF).toByte(),
                    ((longLen ushr 32) and 0xFF).toByte(),
                    ((longLen ushr 24) and 0xFF).toByte(),
                    ((longLen ushr 16) and 0xFF).toByte(),
                    ((longLen ushr 8) and 0xFF).toByte(),
                    (longLen and 0xFF).toByte()
                )
            }
        }
        out.write(header)
        if (len > 0) {
            out.write(payload)
        }
        out.flush()
    }

    /** Reads one WebSocket frame from [input], returning null cleanly on stream EOF. */
    fun readFrame(input: InputStream, maxPayloadBytes: Int = 4 * 1024 * 1024): WsFrame? {
        val b0 = input.read()
        if (b0 < 0) return null
        val b1 = input.read()
        if (b1 < 0) throw EOFException("Unexpected EOF reading WebSocket frame header")

        val fin = (b0 and 0x80) != 0
        val opcode = b0 and 0x0F
        val masked = (b1 and 0x80) != 0
        val len7 = b1 and 0x7F

        val payloadLen: Int = when (len7) {
            126 -> {
                val ext = readExact(input, 2)
                ((ext[0].toInt() and 0xFF) shl 8) or (ext[1].toInt() and 0xFF)
            }

            127 -> {
                val ext = readExact(input, 8)
                var len64 = 0L
                for (i in 0 until 8) {
                    len64 = (len64 shl 8) or (ext[i].toLong() and 0xFFL)
                }
                if (len64 < 0L || len64 > maxPayloadBytes.toLong()) {
                    throw IllegalArgumentException("WebSocket payload exceeds limit: $len64")
                }
                len64.toInt()
            }

            else -> len7
        }

        if (payloadLen > maxPayloadBytes) {
            throw IllegalArgumentException("WebSocket payload exceeds limit: $payloadLen")
        }

        val maskKey = if (masked) readExact(input, 4) else null
        val payload = readExact(input, payloadLen)
        if (maskKey != null) {
            for (i in payload.indices) {
                payload[i] = (payload[i].toInt() xor maskKey[i % 4].toInt()).toByte()
            }
        }
        return WsFrame(fin = fin, opcode = opcode, payload = payload)
    }

    private fun readExact(input: InputStream, count: Int): ByteArray {
        val buf = ByteArray(count)
        var offset = 0
        while (offset < count) {
            val read = input.read(buf, offset, count - offset)
            if (read < 0) throw EOFException("Unexpected EOF reading $count bytes")
            offset += read
        }
        return buf
    }
}
