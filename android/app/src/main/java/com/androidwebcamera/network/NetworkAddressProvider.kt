package com.androidwebcamera.network

import com.androidwebcamera.model.NetworkEndpoint
import java.net.Inet4Address
import java.net.NetworkInterface

/** Discovers active IPv4 network interfaces for Wi-Fi, Hotspot, USB Tethering, and ADB tunneling. */
object NetworkAddressProvider {

    fun getEndpoints(port: Int): List<NetworkEndpoint> {
        val result = mutableListOf<NetworkEndpoint>()

        runCatching {
            val interfaces = NetworkInterface.getNetworkInterfaces() ?: return@runCatching
            while (interfaces.hasMoreElements()) {
                val iface = interfaces.nextElement()
                if (!iface.isUp || iface.isLoopback) continue

                val name = iface.name.lowercase()
                if (shouldIgnoreInterface(name)) continue
                val addresses = iface.inetAddresses
                while (addresses.hasMoreElements()) {
                    val addr = addresses.nextElement()
                    if (addr is Inet4Address && !addr.isLoopbackAddress) {
                        val ip = addr.hostAddress ?: continue
                        if (shouldIgnoreIp(name, ip)) continue
                        val (label, isUsb) = classifyInterface(name)
                        result.add(
                            NetworkEndpoint(
                                label = label,
                                interfaceName = iface.name,
                                ip = ip,
                                url = "http://$ip:$port/",
                                isUsb = isUsb
                            )
                        )
                    }
                }
            }
        }

        val sorted = result.sortedWith(
            compareBy<NetworkEndpoint> { it.isUsb }
                .thenBy { it.label }
                .thenBy { it.ip }
        ).toMutableList()

        sorted.add(
            NetworkEndpoint(
                label = "USB (через adb forward / awc-bridge)",
                interfaceName = "adb",
                ip = "127.0.0.1",
                url = "http://127.0.0.1:$port/",
                isUsb = true
            )
        )
        return sorted
    }

    private fun shouldIgnoreInterface(name: String): Boolean =
        name.startsWith("rmnet") ||
            name.startsWith("ccmni") ||
            name.startsWith("v4-rmnet") ||
            name.startsWith("dummy") ||
            name.startsWith("clat") ||
            name.startsWith("lo")

    private fun shouldIgnoreIp(interfaceName: String, ip: String): Boolean =
        interfaceName.startsWith("tun") && ip.startsWith("198.18.")

    private fun classifyInterface(name: String): Pair<String, Boolean> = when {
        name.startsWith("rndis") || name.startsWith("usb") || name.startsWith("ncm") ->
            "USB-модем (RNDIS)" to true

        name.startsWith("ap") || name.startsWith("swlan") || name.contains("p2p") ->
            "Точка доступа Wi-Fi" to false

        name.startsWith("wlan") || name.startsWith("wifi") ->
            "Wi-Fi" to false

        name.startsWith("eth") ->
            "Ethernet" to false

        else -> "Сеть ($name)" to false
    }
}
