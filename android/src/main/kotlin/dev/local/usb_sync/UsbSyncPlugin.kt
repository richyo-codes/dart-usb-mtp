package dev.local.usb_sync

import android.content.Context
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.plugin.common.MethodChannel.Result

class UsbSyncPlugin : FlutterPlugin, MethodCallHandler {
    private lateinit var channel: MethodChannel
    private lateinit var usbManager: UsbManager

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        usbManager = binding.applicationContext.getSystemService(Context.USB_SERVICE) as UsbManager
        channel = MethodChannel(binding.binaryMessenger, "usb_sync")
        channel.setMethodCallHandler(this)
    }

    override fun onMethodCall(call: MethodCall, result: Result) {
        when (call.method) {
            "listDevices" -> result.success(listDevices(call.arguments as? Map<*, *>))
            "listEntries" -> result.success(emptyList<Map<String, Any?>>())
            "closeSession" -> result.success(null)
            else -> result.error(
                "unimplemented",
                "Method ${call.method} is not implemented for Android yet.",
                null
            )
        }
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        channel.setMethodCallHandler(null)
    }

    private fun listDevices(arguments: Map<*, *>?): List<Map<String, Any?>> {
        val filter = DeviceFilter.fromArgs(arguments)
        return usbManager.deviceList.values.mapNotNull { device ->
            val transports = detectTransports(device)
            val capabilities = detectCapabilities(transports)
            if (!matchesFilter(device, filter, transports, capabilities)) {
                return@mapNotNull null
            }
            mapOf(
                "id" to device.deviceName,
                "vendorId" to device.vendorId,
                "productId" to device.productId,
                "manufacturerName" to safeString { device.manufacturerName },
                "productName" to safeString { device.productName },
                "serialNumber" to safeString { device.serialNumber },
                "transports" to transports.toList(),
                "capabilities" to capabilities.toList(),
                "isMounted" to null,
                "mountPath" to null,
            )
        }
    }

    private fun matchesFilter(
        device: UsbDevice,
        filter: DeviceFilter,
        transports: Set<String>,
        capabilities: Set<String>,
    ): Boolean {
        if (filter.vendorIds.isNotEmpty() && !filter.vendorIds.contains(device.vendorId)) {
            return false
        }
        if (filter.productIds.isNotEmpty() && !filter.productIds.contains(device.productId)) {
            return false
        }
        if (filter.transports.isNotEmpty() && filter.transports.intersect(transports).isEmpty()) {
            return false
        }
        if (filter.requiredCapabilities.any { !capabilities.contains(it) }) {
            return false
        }

        val productName = safeString { device.productName }
        val manufacturerName = safeString { device.manufacturerName }
        if (!matchesPattern(productName, filter.productNamePattern)) {
            return false
        }
        if (!matchesPattern(manufacturerName, filter.manufacturerPattern)) {
            return false
        }
        return true
    }

    private fun detectTransports(device: UsbDevice): Set<String> {
        val transports = linkedSetOf<String>()

        if (device.deviceClass == UsbConstants.USB_CLASS_STILL_IMAGE) {
            transports.add("mtp")
        }
        if (device.deviceClass == UsbConstants.USB_CLASS_MASS_STORAGE) {
            transports.add("massStorage")
        }

        for (index in 0 until device.interfaceCount) {
            val usbInterface = device.getInterface(index)
            if (usbInterface.interfaceClass == UsbConstants.USB_CLASS_STILL_IMAGE) {
                transports.add("mtp")
            }
            if (usbInterface.interfaceClass == UsbConstants.USB_CLASS_MASS_STORAGE) {
                transports.add("massStorage")
            }
        }

        if (transports.isEmpty()) {
            transports.add("unknown")
        }
        return transports
    }

    private fun detectCapabilities(transports: Set<String>): Set<String> {
        val capabilities = linkedSetOf("enumerate")
        if (transports.contains("mtp") || transports.contains("massStorage")) {
            capabilities.add("readFiles")
        }
        return capabilities
    }

    private fun matchesPattern(value: String?, pattern: String?): Boolean {
        if (pattern.isNullOrBlank()) {
            return true
        }
        if (value.isNullOrBlank()) {
            return false
        }
        return try {
            Regex(pattern, RegexOption.IGNORE_CASE).containsMatchIn(value)
        } catch (_: Exception) {
            value.contains(pattern, ignoreCase = true)
        }
    }

    private fun safeString(reader: () -> String?): String? {
        return try {
            reader()
        } catch (_: Exception) {
            null
        }
    }
}

private data class DeviceFilter(
    val vendorIds: Set<Int>,
    val productIds: Set<Int>,
    val transports: Set<String>,
    val requiredCapabilities: Set<String>,
    val productNamePattern: String?,
    val manufacturerPattern: String?,
) {
    companion object {
        fun fromArgs(arguments: Map<*, *>?): DeviceFilter {
            if (arguments == null) {
                return DeviceFilter(
                    vendorIds = emptySet(),
                    productIds = emptySet(),
                    transports = emptySet(),
                    requiredCapabilities = emptySet(),
                    productNamePattern = null,
                    manufacturerPattern = null,
                )
            }
            return DeviceFilter(
                vendorIds = toIntSet(arguments["vendorIds"]),
                productIds = toIntSet(arguments["productIds"]),
                transports = toStringSet(arguments["transports"]),
                requiredCapabilities = toStringSet(arguments["requiredCapabilities"]),
                productNamePattern = arguments["productNamePattern"] as? String,
                manufacturerPattern = arguments["manufacturerPattern"] as? String,
            )
        }

        private fun toIntSet(raw: Any?): Set<Int> {
            val list = raw as? List<*> ?: return emptySet()
            return list.mapNotNull { (it as? Number)?.toInt() }.toSet()
        }

        private fun toStringSet(raw: Any?): Set<String> {
            val list = raw as? List<*> ?: return emptySet()
            return list.mapNotNull { it as? String }.toSet()
        }
    }
}
