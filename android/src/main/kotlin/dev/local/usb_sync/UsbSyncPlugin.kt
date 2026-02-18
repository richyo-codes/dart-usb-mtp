package dev.local.usb_sync

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.mtp.MtpDevice
import android.mtp.MtpObjectInfo
import android.os.Build
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.plugin.common.MethodChannel.Result
import java.util.Locale

private const val MTP_FORMAT_ASSOCIATION = 0x3001
private const val USB_PERMISSION_ACTION = "dev.local.usb_sync.USB_PERMISSION"

class UsbSyncPlugin : FlutterPlugin, MethodCallHandler {
    private lateinit var channel: MethodChannel
    private lateinit var appContext: Context
    private lateinit var usbManager: UsbManager

    private val sessions = linkedMapOf<String, UsbSessionState>()
    private val pendingOpenResults = linkedMapOf<String, MutableList<Result>>()
    private var nextSessionOrdinal: Long = 1
    private var receiverRegistered = false

    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != USB_PERMISSION_ACTION) {
                return
            }
            val device = parcelableUsbDevice(intent) ?: return
            val pending = pendingOpenResults.remove(device.deviceName) ?: return
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            if (!granted) {
                pending.forEach {
                    it.error(
                        "permission_denied",
                        "USB permission was denied for device ${device.deviceName}.",
                        null,
                    )
                }
                return
            }
            pending.forEach { openDeviceWithPermission(device, it) }
        }
    }

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        appContext = binding.applicationContext
        usbManager = appContext.getSystemService(Context.USB_SERVICE) as UsbManager
        channel = MethodChannel(binding.binaryMessenger, "usb_sync")
        channel.setMethodCallHandler(this)
        registerPermissionReceiver()
    }

    override fun onMethodCall(call: MethodCall, result: Result) {
        when (call.method) {
            "listDevices" -> result.success(listDevices(call.arguments as? Map<*, *>))
            "openDevice" -> openDevice(call.arguments as? Map<*, *>, result)
            "listEntries" -> listEntries(call.arguments as? Map<*, *>, result)
            "readBytes" -> readBytes(call.arguments as? Map<*, *>, result)
            "closeSession" -> closeSession(call.arguments as? Map<*, *>, result)
            else -> result.error(
                "unimplemented",
                "Method ${call.method} is not implemented for Android.",
                null,
            )
        }
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        channel.setMethodCallHandler(null)
        pendingOpenResults.values.flatten().forEach {
            it.error("channel_detached", "usb_sync plugin detached.", null)
        }
        pendingOpenResults.clear()
        closeAllSessions()
        unregisterPermissionReceiver()
    }

    private fun registerPermissionReceiver() {
        if (receiverRegistered) {
            return
        }
        val filter = IntentFilter(USB_PERMISSION_ACTION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            appContext.registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            appContext.registerReceiver(permissionReceiver, filter)
        }
        receiverRegistered = true
    }

    private fun unregisterPermissionReceiver() {
        if (!receiverRegistered) {
            return
        }
        try {
            appContext.unregisterReceiver(permissionReceiver)
        } catch (_: Exception) {
        }
        receiverRegistered = false
    }

    private fun listDevices(arguments: Map<*, *>?): List<Map<String, Any?>> {
        val filter = DeviceFilter.fromArgs(arguments)
        return usbManager.deviceList.values
            .sortedBy { it.deviceName }
            .mapNotNull { device ->
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

    private fun openDevice(arguments: Map<*, *>?, result: Result) {
        val deviceId = arguments?.get("deviceId") as? String
        if (deviceId.isNullOrBlank()) {
            result.error("invalid_args", "Missing required argument: deviceId.", null)
            return
        }

        val device = usbManager.deviceList[deviceId]
        if (device == null) {
            result.error("device_not_found", "Device not found: $deviceId", null)
            return
        }

        val transports = detectTransports(device)
        if (!transports.contains("mtp")) {
            result.error(
                "unsupported_transport",
                "Android usb_sync currently supports only MTP sessions.",
                null,
            )
            return
        }

        if (usbManager.hasPermission(device)) {
            openDeviceWithPermission(device, result)
            return
        }

        val queue = pendingOpenResults.getOrPut(device.deviceName) { mutableListOf() }
        queue.add(result)
        requestPermission(device)
    }

    private fun requestPermission(device: UsbDevice) {
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        } else {
            PendingIntent.FLAG_UPDATE_CURRENT
        }
        val intent = Intent(USB_PERMISSION_ACTION).setPackage(appContext.packageName)
        val pendingIntent = PendingIntent.getBroadcast(appContext, device.deviceId, intent, flags)
        try {
            usbManager.requestPermission(device, pendingIntent)
        } catch (error: Exception) {
            val pending = pendingOpenResults.remove(device.deviceName)
            pending?.forEach {
                it.error(
                    "permission_request_failed",
                    error.message ?: "Unable to request USB permission.",
                    null,
                )
            }
        }
    }

    private fun openDeviceWithPermission(device: UsbDevice, result: Result) {
        val connection = usbManager.openDevice(device)
        if (connection == null) {
            result.error("open_failed", "Unable to open USB connection for ${device.deviceName}.", null)
            return
        }

        val mtpDevice = MtpDevice(device)
        val opened = try {
            mtpDevice.open(connection)
        } catch (_: Exception) {
            false
        }
        if (!opened) {
            closeQuietly(connection)
            result.error("open_failed", "Unable to open MTP session for ${device.deviceName}.", null)
            return
        }

        val sessionId = nextSessionToken(device.deviceName)
        sessions[sessionId] = UsbSessionState(
            sessionId = sessionId,
            deviceId = device.deviceName,
            device = device,
            connection = connection,
            mtpDevice = mtpDevice,
        )
        result.success(
            mapOf(
                "sessionId" to sessionId,
                "deviceId" to device.deviceName,
            ),
        )
    }

    private fun listEntries(arguments: Map<*, *>?, result: Result) {
        val sessionId = arguments?.get("sessionId") as? String
        val path = arguments?.get("path") as? String ?: "/"
        if (sessionId.isNullOrBlank()) {
            result.error("invalid_args", "Missing required argument: sessionId.", null)
            return
        }
        val session = sessions[sessionId]
        if (session == null) {
            result.error("session_not_found", "Session does not exist or is closed.", null)
            return
        }

        try {
            val entries = listEntriesForPath(session, normalizePath(path))
            result.success(entries)
        } catch (error: IllegalArgumentException) {
            result.error("invalid_path", error.message, null)
        } catch (error: Exception) {
            result.error("list_entries_failed", error.message, null)
        }
    }

    private fun readBytes(arguments: Map<*, *>?, result: Result) {
        val sessionId = arguments?.get("sessionId") as? String
        val path = arguments?.get("path") as? String
        if (sessionId.isNullOrBlank() || path.isNullOrBlank()) {
            result.error(
                "invalid_args",
                "Missing required arguments: sessionId and path.",
                null,
            )
            return
        }

        val session = sessions[sessionId]
        if (session == null) {
            result.error("session_not_found", "Session does not exist or is closed.", null)
            return
        }

        val resolved = resolvePath(session, normalizePath(path))
        if (resolved == null || resolved.isDirectory || resolved.storageId == null || resolved.handle <= 0) {
            result.error("invalid_path", "Path is not a file: $path", null)
            return
        }

        val info = safeObjectInfo(session.mtpDevice, resolved.handle)
        if (info == null) {
            result.error("read_failed", "Unable to read metadata for path: $path", null)
            return
        }

        val sizeLong = info.compressedSize
        if (sizeLong < 0) {
            result.error("read_failed", "Invalid object size for path: $path", null)
            return
        }
        if (sizeLong > Int.MAX_VALUE.toLong()) {
            result.error("file_too_large", "File is too large for a single read on Android MTP.", null)
            return
        }
        if (sizeLong == 0L) {
            result.success(ByteArray(0))
            return
        }

        val bytes = try {
            session.mtpDevice.getObject(resolved.handle, sizeLong.toInt())
        } catch (_: Exception) {
            null
        }
        if (bytes == null) {
            result.error("read_failed", "Unable to read file bytes for path: $path", null)
            return
        }

        result.success(bytes)
    }

    private fun closeSession(arguments: Map<*, *>?, result: Result) {
        val sessionId = arguments?.get("sessionId") as? String
        if (sessionId.isNullOrBlank()) {
            result.error("invalid_args", "Missing required argument: sessionId.", null)
            return
        }
        val session = sessions.remove(sessionId)
        if (session != null) {
            closeSessionInternal(session)
        }
        result.success(null)
    }

    private fun closeAllSessions() {
        val all = sessions.values.toList()
        sessions.clear()
        all.forEach { closeSessionInternal(it) }
    }

    private fun closeSessionInternal(session: UsbSessionState) {
        closeQuietly(session.mtpDevice)
        closeQuietly(session.connection)
    }

    private fun closeQuietly(device: MtpDevice) {
        try {
            device.close()
        } catch (_: Exception) {
        }
    }

    private fun closeQuietly(connection: UsbDeviceConnection) {
        try {
            connection.close()
        } catch (_: Exception) {
        }
    }

    private fun nextSessionToken(deviceId: String): String {
        val id = nextSessionOrdinal
        nextSessionOrdinal += 1
        return "android:$deviceId:$id"
    }

    private fun listEntriesForPath(
        session: UsbSessionState,
        normalizedPath: String,
    ): List<Map<String, Any?>> {
        val node = resolvePath(session, normalizedPath)
            ?: throw IllegalArgumentException("Path does not exist: $normalizedPath")
        if (!node.isDirectory) {
            throw IllegalArgumentException("Path is not a directory: $normalizedPath")
        }

        if (node.path == "/") {
            return storageRoots(session)
                .map { root ->
                    mapOf(
                        "path" to root.path,
                        "name" to root.label,
                        "isDirectory" to true,
                        "sizeBytes" to null,
                        "modifiedAtMs" to null,
                    )
                }
                .sortedBy { (it["name"] as? String).orEmpty().lowercase(Locale.ROOT) }
        }

        val storageId = node.storageId
            ?: throw IllegalArgumentException("Directory is missing storage context: $normalizedPath")

        return listChildren(session, storageId, node.handle, node.path)
            .map { child ->
                mapOf(
                    "path" to child.path,
                    "name" to child.name,
                    "isDirectory" to child.isDirectory,
                    "sizeBytes" to child.sizeBytes,
                    "modifiedAtMs" to child.modifiedAtMs,
                )
            }
    }

    private fun resolvePath(session: UsbSessionState, path: String): ResolvedNode? {
        val normalized = normalizePath(path)
        if (normalized == "/") {
            return ResolvedNode(
                storageId = null,
                handle = 0,
                path = "/",
                name = "/",
                isDirectory = true,
                sizeBytes = null,
                modifiedAtMs = null,
            )
        }

        val segments = splitPath(normalized)
        if (segments.isEmpty()) {
            return null
        }

        val roots = storageRoots(session)
        val firstSegment = segments.first()
        val root = roots.firstOrNull { it.label == firstSegment }
            ?: roots.firstOrNull { it.label.equals(firstSegment, ignoreCase = true) }
            ?: return null

        var current = ResolvedNode(
            storageId = root.storageId,
            handle = 0,
            path = root.path,
            name = root.label,
            isDirectory = true,
            sizeBytes = null,
            modifiedAtMs = null,
        )

        for (segment in segments.drop(1)) {
            val children = listChildren(session, root.storageId, current.handle, current.path)
            val next = children.firstOrNull { it.name == segment }
                ?: children.firstOrNull { it.name.equals(segment, ignoreCase = true) }
                ?: return null
            current = next
        }

        return current
    }

    private fun listChildren(
        session: UsbSessionState,
        storageId: Int,
        parentHandle: Int,
        parentPath: String,
    ): List<ResolvedNode> {
        val handles = try {
            session.mtpDevice.getObjectHandles(storageId, 0, parentHandle) ?: IntArray(0)
        } catch (_: Exception) {
            IntArray(0)
        }

        val children = ArrayList<ResolvedNode>(handles.size)
        for (handle in handles) {
            val info = safeObjectInfo(session.mtpDevice, handle) ?: continue
            val rawName = info.name?.trim().orEmpty()
            val name = rawName.ifEmpty { "object_$handle" }
            val childPath = if (parentPath == "/") {
                "/$name"
            } else {
                "$parentPath/$name"
            }
            val isDirectory = info.format == MTP_FORMAT_ASSOCIATION
            val sizeBytes = if (isDirectory) {
                null
            } else {
                toIntOrNull(info.compressedSize)
            }
            children.add(
                ResolvedNode(
                    storageId = storageId,
                    handle = handle,
                    path = childPath,
                    name = name,
                    isDirectory = isDirectory,
                    sizeBytes = sizeBytes,
                    modifiedAtMs = toEpochMillisOrNull(info.dateModified),
                ),
            )
        }

        children.sortWith(
            compareBy<ResolvedNode>({ !it.isDirectory }, { it.name.lowercase(Locale.ROOT) }),
        )
        return children
    }

    private fun storageRoots(session: UsbSessionState): List<StorageRoot> {
        session.storageRoots?.let { return it }

        val ids = try {
            session.mtpDevice.storageIds ?: IntArray(0)
        } catch (_: Exception) {
            IntArray(0)
        }

        val roots = mutableListOf<StorageRoot>()
        val seen = mutableSetOf<String>()
        var fallbackIndex = 1
        for (storageId in ids) {
            val info = try {
                session.mtpDevice.getStorageInfo(storageId)
            } catch (_: Exception) {
                null
            }
            val rawLabel = info?.description?.trim().orEmpty()
            val sanitized = sanitizeRootLabel(rawLabel)
            val baseLabel = if (sanitized.isNotEmpty()) {
                sanitized
            } else {
                "Storage $fallbackIndex"
            }
            var label = baseLabel
            var suffix = 2
            while (!seen.add(label.lowercase(Locale.ROOT))) {
                label = "$baseLabel ($suffix)"
                suffix += 1
            }
            roots.add(StorageRoot(storageId = storageId, label = label, path = "/$label"))
            fallbackIndex += 1
        }

        roots.sortBy { it.label.lowercase(Locale.ROOT) }
        session.storageRoots = roots
        return roots
    }

    private fun toIntOrNull(value: Long): Int? {
        if (value < 0L || value > Int.MAX_VALUE.toLong()) {
            return null
        }
        return value.toInt()
    }

    private fun toEpochMillisOrNull(value: Long): Long? {
        if (value <= 0L) {
            return null
        }
        val maxSeconds = Long.MAX_VALUE / 1000L
        if (value > maxSeconds) {
            return null
        }
        return value * 1000L
    }

    private fun normalizePath(path: String?): String {
        val raw = path?.trim().orEmpty()
        if (raw.isEmpty()) {
            return "/"
        }
        var normalized = raw.replace('\\', '/')
        if (!normalized.startsWith('/')) {
            normalized = "/$normalized"
        }
        while (normalized.contains("//")) {
            normalized = normalized.replace("//", "/")
        }
        while (normalized.length > 1 && normalized.endsWith('/')) {
            normalized = normalized.dropLast(1)
        }
        return normalized
    }

    private fun splitPath(path: String): List<String> {
        return normalizePath(path)
            .split('/')
            .filter { it.isNotBlank() }
    }

    private fun sanitizeRootLabel(input: String): String {
        return input
            .replace('/', '_')
            .replace('\\', '_')
            .trim()
    }

    private fun safeObjectInfo(mtpDevice: MtpDevice, handle: Int): MtpObjectInfo? {
        return try {
            mtpDevice.getObjectInfo(handle)
        } catch (_: Exception) {
            null
        }
    }

    private fun parcelableUsbDevice(intent: Intent): UsbDevice? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE) as? UsbDevice
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
        if (transports.contains("mtp")) {
            capabilities.add("mount")
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

private data class UsbSessionState(
    val sessionId: String,
    val deviceId: String,
    val device: UsbDevice,
    val connection: UsbDeviceConnection,
    val mtpDevice: MtpDevice,
    var storageRoots: List<StorageRoot>? = null,
)

private data class StorageRoot(
    val storageId: Int,
    val label: String,
    val path: String,
)

private data class ResolvedNode(
    val storageId: Int?,
    val handle: Int,
    val path: String,
    val name: String,
    val isDirectory: Boolean,
    val sizeBytes: Int?,
    val modifiedAtMs: Long?,
)

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
