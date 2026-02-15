enum UsbTransport { mtp, massStorage, unknown }

enum UsbCapability { enumerate, readFiles, mount, watchEvents, unknown }

enum UsbDeviceEventType { attached, detached, changed }

class UsbFilter {
  final List<int> vendorIds;
  final List<int> productIds;
  final List<UsbTransport> transports;
  final List<UsbCapability> requiredCapabilities;
  final String? productNamePattern;
  final String? manufacturerPattern;

  const UsbFilter({
    this.vendorIds = const <int>[],
    this.productIds = const <int>[],
    this.transports = const <UsbTransport>[],
    this.requiredCapabilities = const <UsbCapability>[],
    this.productNamePattern,
    this.manufacturerPattern,
  });

  Map<String, Object?> toMap() {
    return <String, Object?>{
      "vendorIds": vendorIds,
      "productIds": productIds,
      "transports": transports.map((UsbTransport x) => x.name).toList(),
      "requiredCapabilities":
          requiredCapabilities.map((UsbCapability x) => x.name).toList(),
      "productNamePattern": productNamePattern,
      "manufacturerPattern": manufacturerPattern,
    };
  }
}

class UsbDeviceInfo {
  final String id;
  final int? vendorId;
  final int? productId;
  final String? manufacturerName;
  final String? productName;
  final String? serialNumber;
  final List<UsbTransport> transports;
  final List<UsbCapability> capabilities;
  final bool? isMounted;
  final String? mountPath;

  const UsbDeviceInfo({
    required this.id,
    this.vendorId,
    this.productId,
    this.manufacturerName,
    this.productName,
    this.serialNumber,
    this.transports = const <UsbTransport>[],
    this.capabilities = const <UsbCapability>[],
    this.isMounted,
    this.mountPath,
  });

  factory UsbDeviceInfo.fromMap(Map<String, Object?> map) {
    final List<dynamic> transportsRaw =
        (map["transports"] as List<dynamic>? ?? const <dynamic>[]);
    final List<dynamic> capabilitiesRaw =
        (map["capabilities"] as List<dynamic>? ?? const <dynamic>[]);

    return UsbDeviceInfo(
      id: map["id"] as String,
      vendorId: map["vendorId"] as int?,
      productId: map["productId"] as int?,
      manufacturerName: map["manufacturerName"] as String?,
      productName: map["productName"] as String?,
      serialNumber: map["serialNumber"] as String?,
      transports: transportsRaw
          .map(
            (dynamic value) => _transportFromString(value.toString()),
          )
          .toList(growable: false),
      capabilities: capabilitiesRaw
          .map(
            (dynamic value) => _capabilityFromString(value.toString()),
          )
          .toList(growable: false),
      isMounted: map["isMounted"] as bool?,
      mountPath: map["mountPath"] as String?,
    );
  }
}

class UsbSession {
  final String sessionId;
  final String deviceId;

  const UsbSession({
    required this.sessionId,
    required this.deviceId,
  });

  factory UsbSession.fromMap(Map<String, Object?> map) {
    return UsbSession(
      sessionId: map["sessionId"] as String,
      deviceId: map["deviceId"] as String,
    );
  }
}

class UsbEntry {
  final String path;
  final String name;
  final bool isDirectory;
  final int? sizeBytes;
  final int? modifiedAtMs;

  const UsbEntry({
    required this.path,
    required this.name,
    required this.isDirectory,
    this.sizeBytes,
    this.modifiedAtMs,
  });

  factory UsbEntry.fromMap(Map<String, Object?> map) {
    return UsbEntry(
      path: map["path"] as String,
      name: map["name"] as String,
      isDirectory: map["isDirectory"] as bool? ?? false,
      sizeBytes: map["sizeBytes"] as int?,
      modifiedAtMs: map["modifiedAtMs"] as int?,
    );
  }
}

class UsbDeviceEvent {
  final UsbDeviceEventType type;
  final UsbDeviceInfo device;

  const UsbDeviceEvent({
    required this.type,
    required this.device,
  });

  factory UsbDeviceEvent.fromMap(Map<String, Object?> map) {
    return UsbDeviceEvent(
      type: _eventTypeFromString((map["type"] as String?) ?? "changed"),
      device: UsbDeviceInfo.fromMap(
        Map<String, Object?>.from(
          (map["device"] as Map<dynamic, dynamic>? ?? <dynamic, dynamic>{}),
        ),
      ),
    );
  }
}

UsbTransport _transportFromString(String value) {
  switch (value) {
    case "mtp":
      return UsbTransport.mtp;
    case "massStorage":
      return UsbTransport.massStorage;
    default:
      return UsbTransport.unknown;
  }
}

UsbCapability _capabilityFromString(String value) {
  switch (value) {
    case "enumerate":
      return UsbCapability.enumerate;
    case "readFiles":
      return UsbCapability.readFiles;
    case "mount":
      return UsbCapability.mount;
    case "watchEvents":
      return UsbCapability.watchEvents;
    default:
      return UsbCapability.unknown;
  }
}

UsbDeviceEventType _eventTypeFromString(String value) {
  switch (value) {
    case "attached":
      return UsbDeviceEventType.attached;
    case "detached":
      return UsbDeviceEventType.detached;
    default:
      return UsbDeviceEventType.changed;
  }
}
