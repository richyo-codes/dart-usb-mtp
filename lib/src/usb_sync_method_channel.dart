import "dart:async";

import "package:flutter/services.dart";

import "models.dart";
import "usb_sync_exception.dart";
import "usb_sync_platform_interface.dart";

class MethodChannelUsbSync extends UsbSyncPlatform {
  static const MethodChannel _channel = MethodChannel("usb_mtp_client");

  @override
  Future<List<UsbDeviceInfo>> listDevices({
    UsbFilter filter = const UsbFilter(),
  }) async {
    final List<dynamic>? raw = await _channel.invokeMethod<List<dynamic>>(
      "listDevices",
      filter.toMap(),
    );
    if (raw == null) {
      return const <UsbDeviceInfo>[];
    }
    return raw
        .map(
          (dynamic item) => UsbDeviceInfo.fromMap(
            Map<String, Object?>.from(item as Map<dynamic, dynamic>),
          ),
        )
        .toList(growable: false);
  }

  @override
  Stream<UsbDeviceEvent> watchDevices({UsbFilter filter = const UsbFilter()}) {
    late StreamController<UsbDeviceEvent> controller;
    Timer? timer;
    bool inFlight = false;
    Map<String, UsbDeviceInfo> previousById = <String, UsbDeviceInfo>{};

    Future<void> poll() async {
      if (inFlight) {
        return;
      }
      inFlight = true;
      try {
        final currentDevices = await listDevices(filter: filter);
        final currentById = <String, UsbDeviceInfo>{
          for (final device in currentDevices) device.id: device,
        };

        for (final entry in currentById.entries) {
          final old = previousById[entry.key];
          if (old == null) {
            controller.add(
              UsbDeviceEvent(
                type: UsbDeviceEventType.attached,
                device: entry.value,
              ),
            );
          } else if (!_sameDevice(old, entry.value)) {
            controller.add(
              UsbDeviceEvent(
                type: UsbDeviceEventType.changed,
                device: entry.value,
              ),
            );
          }
        }

        for (final entry in previousById.entries) {
          if (!currentById.containsKey(entry.key)) {
            controller.add(
              UsbDeviceEvent(
                type: UsbDeviceEventType.detached,
                device: entry.value,
              ),
            );
          }
        }

        previousById = currentById;
      } catch (error, stackTrace) {
        if (!controller.isClosed) {
          controller.addError(error, stackTrace);
        }
      } finally {
        inFlight = false;
      }
    }

    controller = StreamController<UsbDeviceEvent>(
      onListen: () {
        unawaited(poll());
        timer = Timer.periodic(const Duration(seconds: 2), (_) {
          unawaited(poll());
        });
      },
      onCancel: () {
        timer?.cancel();
        timer = null;
      },
    );

    return controller.stream;
  }

  @override
  Future<UsbSession> openDevice({required String deviceId}) async {
    final Map<dynamic, dynamic>? raw = await _channel
        .invokeMethod<Map<dynamic, dynamic>>("openDevice", <String, Object?>{
          "deviceId": deviceId,
        });
    if (raw == null) {
      throw const UsbSyncException(
        code: "null_response",
        message: "openDevice returned null.",
      );
    }
    return UsbSession.fromMap(Map<String, Object?>.from(raw));
  }

  @override
  Future<List<UsbEntry>> listEntries({
    required String sessionId,
    String path = "/",
  }) async {
    final List<dynamic>? raw = await _channel.invokeMethod<List<dynamic>>(
      "listEntries",
      <String, Object?>{"sessionId": sessionId, "path": path},
    );
    if (raw == null) {
      return const <UsbEntry>[];
    }
    return raw
        .map(
          (dynamic item) => UsbEntry.fromMap(
            Map<String, Object?>.from(item as Map<dynamic, dynamic>),
          ),
        )
        .toList(growable: false);
  }

  @override
  Future<Uint8List> readBytes({
    required String sessionId,
    required String path,
  }) async {
    final Uint8List? data = await _channel.invokeMethod<Uint8List>(
      "readBytes",
      <String, Object?>{"sessionId": sessionId, "path": path},
    );
    return data ?? Uint8List(0);
  }

  @override
  Future<void> closeSession({required String sessionId}) async {
    await _channel.invokeMethod<void>("closeSession", <String, Object?>{
      "sessionId": sessionId,
    });
  }
}

bool _sameDevice(UsbDeviceInfo a, UsbDeviceInfo b) {
  return a.id == b.id &&
      a.vendorId == b.vendorId &&
      a.productId == b.productId &&
      a.manufacturerName == b.manufacturerName &&
      a.productName == b.productName &&
      a.serialNumber == b.serialNumber &&
      a.isMounted == b.isMounted &&
      a.mountPath == b.mountPath &&
      _sameEnumSet(a.transports, b.transports) &&
      _sameEnumSet(a.capabilities, b.capabilities);
}

bool _sameEnumSet<T extends Enum>(List<T> a, List<T> b) {
  if (a.length != b.length) {
    return false;
  }
  final aSet = a.map((value) => value.name).toSet();
  final bSet = b.map((value) => value.name).toSet();
  if (aSet.length != bSet.length) {
    return false;
  }
  return aSet.containsAll(bSet);
}
