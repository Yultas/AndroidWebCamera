package com.androidwebcamera.camera

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test
import java.nio.ByteBuffer

class YuvConverterTest {

    @Test
    fun convertsPackedPlanarI420ToNv21Accurately() {
        val w = 4
        val h = 4
        val y = ByteArray(w * h) { (it + 10).toByte() }
        val u = byteArrayOf(0x11, 0x12, 0x13, 0x14)
        val v = byteArrayOf(0x21, 0x22, 0x23, 0x24)

        val out = ByteArray(w * h * 3 / 2)
        val converter = YuvConverter()
        converter.yuv420ToNv21(
            width = w,
            height = h,
            yBuffer = ByteBuffer.wrap(y),
            yRowStride = w,
            yPixelStride = 1,
            uBuffer = ByteBuffer.wrap(u),
            uRowStride = w / 2,
            uPixelStride = 1,
            vBuffer = ByteBuffer.wrap(v),
            vRowStride = w / 2,
            vPixelStride = 1,
            out = out
        )

        assertArrayEquals(y, out.copyOfRange(0, w * h))
        val expectedVu = byteArrayOf(
            0x21, 0x11, 0x22, 0x12,
            0x23, 0x13, 0x24, 0x14
        )
        assertArrayEquals(expectedVu, out.copyOfRange(w * h, out.size))
    }

    @Test
    fun convertsPaddedSemiPlanarStridesWithoutOutOfBounds() {
        val w = 4
        val h = 2
        val yStride = 8 // 4 bytes padding per row
        val yRaw = ByteArray(yStride * h) { 0x7F }
        for (row in 0 until h) {
            for (col in 0 until w) {
                yRaw[row * yStride + col] = (row * 10 + col + 1).toByte()
            }
        }
        // Semi-planar interleaved UV with pixelStride = 2 and rowStride = 8
        val uRaw = byteArrayOf(0x31, 0x41, 0x32, 0x42, 0, 0, 0, 0)
        val vRaw = byteArrayOf(0x41, 0x32, 0x42) // intentionally short trailing byte (common on Android HAL)

        val out = ByteArray(w * h * 3 / 2)
        val converter = YuvConverter()
        converter.yuv420ToNv21(
            width = w,
            height = h,
            yBuffer = ByteBuffer.wrap(yRaw),
            yRowStride = yStride,
            yPixelStride = 1,
            uBuffer = ByteBuffer.wrap(uRaw),
            uRowStride = 8,
            uPixelStride = 2,
            vBuffer = ByteBuffer.wrap(vRaw),
            vRowStride = 8,
            vPixelStride = 2,
            out = out
        )

        assertArrayEquals(byteArrayOf(1, 2, 3, 4, 11, 12, 13, 14), out.copyOfRange(0, 8))
        assertEquals(0x41.toByte(), out[8])
        assertEquals(0x31.toByte(), out[9])
        assertEquals(0x42.toByte(), out[10])
        assertEquals(0x32.toByte(), out[11])
    }
}
