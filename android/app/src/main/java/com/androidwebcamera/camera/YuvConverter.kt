package com.androidwebcamera.camera

import java.nio.ByteBuffer

/**
 * High-performance, bounds-safe YUV_420_888 to NV21 converter.
 * Bulk-copies native ByteBuffers once per plane into reusable JVM scratch arrays to avoid
 * millions of per-pixel JNI ByteBuffer calls at 1080p/4K.
 */
class YuvConverter {

    private var yScratch = ByteArray(0)
    private var uScratch = ByteArray(0)
    private var vScratch = ByteArray(0)

    fun yuv420ToNv21(
        width: Int,
        height: Int,
        yBuffer: ByteBuffer,
        yRowStride: Int,
        yPixelStride: Int,
        uBuffer: ByteBuffer,
        uRowStride: Int,
        uPixelStride: Int,
        vBuffer: ByteBuffer,
        vRowStride: Int,
        vPixelStride: Int,
        out: ByteArray
    ) {
        val ySize = width * height
        require(out.size >= ySize + (width / 2) * (height / 2) * 2) {
            "Output NV21 buffer too small for ${width}x${height}"
        }

        val yBuf = yBuffer.duplicate()
        val yLimit = yBuf.limit()
        var dstPos = 0

        if (yPixelStride == 1 && yRowStride == width) {
            yBuf.position(0)
            val toCopy = minOf(ySize, yLimit)
            yBuf.get(out, 0, toCopy)
            if (toCopy < ySize) {
                out.fill(0, toCopy, ySize)
            }
            dstPos = ySize
        } else if (yPixelStride == 1) {
            for (row in 0 until height) {
                val rowBase = row * yRowStride
                if (rowBase < yLimit) {
                    yBuf.position(rowBase)
                    val toCopy = minOf(width, yLimit - rowBase)
                    yBuf.get(out, dstPos, toCopy)
                    if (toCopy < width) {
                        out.fill(0, dstPos + toCopy, dstPos + width)
                    }
                } else {
                    out.fill(0, dstPos, dstPos + width)
                }
                dstPos += width
            }
        } else {
            val yLen = copyToScratch(yBuf, ScratchPlane.Y)
            val yArr = yScratch
            for (row in 0 until height) {
                val rowBase = row * yRowStride
                for (col in 0 until width) {
                    val idx = rowBase + col * yPixelStride
                    out[dstPos++] = if (idx < yLen) yArr[idx] else 0
                }
            }
        }

        val chromaHeight = height / 2
        val chromaWidth = width / 2
        val vLen = copyToScratch(vBuffer.duplicate(), ScratchPlane.V)
        val uLen = copyToScratch(uBuffer.duplicate(), ScratchPlane.U)
        val vArr = vScratch
        val uArr = uScratch

        for (row in 0 until chromaHeight) {
            val vRowBase = row * vRowStride
            val uRowBase = row * uRowStride
            for (col in 0 until chromaWidth) {
                val vIdx = vRowBase + col * vPixelStride
                val uIdx = uRowBase + col * uPixelStride
                out[dstPos++] = if (vIdx < vLen) vArr[vIdx] else 128.toByte()
                out[dstPos++] = if (uIdx < uLen) uArr[uIdx] else 128.toByte()
            }
        }
    }

    private enum class ScratchPlane { Y, U, V }

    private fun copyToScratch(buf: ByteBuffer, plane: ScratchPlane): Int {
        buf.position(0)
        val len = buf.remaining()
        var arr = when (plane) {
            ScratchPlane.Y -> yScratch
            ScratchPlane.U -> uScratch
            ScratchPlane.V -> vScratch
        }
        if (arr.size < len) {
            arr = ByteArray(len)
            when (plane) {
                ScratchPlane.Y -> yScratch = arr
                ScratchPlane.U -> uScratch = arr
                ScratchPlane.V -> vScratch = arr
            }
        }
        if (len > 0) {
            buf.get(arr, 0, len)
        }
        return len
    }
}
