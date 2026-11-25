package com.androidwebcamera.camera

import android.view.Surface

/** Common contract for hardware H.264 and YUV->MJPEG video encoders fed from Camera2. */
interface VideoEncoder {
    val inputSurface: Surface
    val hasProducedFrame: Boolean
    fun start()
    fun requestSyncFrame()
    fun updateBitrate(bitrateKbps: Int)
    fun stopAndRelease()
}
