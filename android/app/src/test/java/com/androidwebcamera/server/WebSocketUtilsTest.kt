package com.androidwebcamera.server

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.EOFException

class WebSocketUtilsTest {

    @Test
    fun computeAcceptKeyMatchesRfc6455TestVector() {
        val accept = WebSocketUtils.computeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==")
        assertEquals("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", accept)
    }

    @Test
    fun writeAndReadSmallMediumAndLargeFrames() {
        val sizes = listOf(0, 17, 125, 126, 4096, 70_000)
        for (size in sizes) {
            val payload = ByteArray(size) { idx -> (idx * 31 + 7).toByte() }
            val out = ByteArrayOutputStream()
            WebSocketUtils.writeFrame(out, WebSocketUtils.OPCODE_BINARY, payload)

            val parsed = WebSocketUtils.readFrame(ByteArrayInputStream(out.toByteArray()))
            assertNotNull("Expected parsed frame for size=$size", parsed)
            assertEquals(true, parsed!!.fin)
            assertEquals(WebSocketUtils.OPCODE_BINARY, parsed.opcode)
            assertArrayEquals(payload, parsed.payload)
        }
    }

    @Test
    fun readMaskedClientFrameUnmasksPayload() {
        val plain = "hello-awc".toByteArray()
        val mask = byteArrayOf(0x12, 0x34, 0x56, 0x78)
        val raw = ByteArrayOutputStream().apply {
            write(0x81) // FIN + TEXT
            write(0x80 or plain.size) // MASK + len
            write(mask)
            for (i in plain.indices) {
                write(plain[i].toInt() xor mask[i % 4].toInt())
            }
        }.toByteArray()

        val parsed = WebSocketUtils.readFrame(ByteArrayInputStream(raw))!!
        assertEquals(WebSocketUtils.OPCODE_TEXT, parsed.opcode)
        assertArrayEquals(plain, parsed.payload)
    }

    @Test
    fun readFrameOnEmptyStreamReturnsNullAndRejectsTruncatedOrOversized() {
        assertNull(WebSocketUtils.readFrame(ByteArrayInputStream(ByteArray(0))))

        // Truncated header
        assertThrows(EOFException::class.java) {
            WebSocketUtils.readFrame(ByteArrayInputStream(byteArrayOf(0x82.toByte())))
        }

        // Payload exceeding maxPayloadBytes
        val out = ByteArrayOutputStream()
        WebSocketUtils.writeFrame(out, WebSocketUtils.OPCODE_BINARY, ByteArray(1024))
        assertThrows(IllegalArgumentException::class.java) {
            WebSocketUtils.readFrame(ByteArrayInputStream(out.toByteArray()), maxPayloadBytes = 128)
        }
    }
}
