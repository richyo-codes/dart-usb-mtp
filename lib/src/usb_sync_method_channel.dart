import "package:flutter/services.dart";

import "models.dart";
import "usb_sync_exception.dart";
import "usb_sync_platform_interface.dart";

class MethodChannelUsbSync extends UsbSyncPlatform {
  static const MethodChannel _channel = MethodChannel("usb_sync");

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
  Stream<UsbDeviceEvent> watchDevices({
    UsbFilter filter = const UsbFilter(),
  }) {
    return const Stream<UsbDeviceEvent>.empty();
  }

  @override
  Future<UsbSession> openDevice({
    required String deviceId,
  }) async {
    final Map<dynamic, dynamic>? raw = await _channel
        .invokeMethod<Map<dynamic, dynamic>>(
          "openDevice",
          <String, Object?>{"deviceId": deviceId},
        );
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
      <String, Object?>{
        "sessionId": sessionId,
        "path": path,
      },
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
      <String, Object?>{
        "sessionId": sessionId,
        "path": path,
      },
    );
    return data ?? Uint8List(0);
  }

  @override
  Future<void> closeSession({
    required String sessionId,
  }) async {
    await _channel.invokeMethod<void>(
      "closeSession",
      <String, Object?>{"sessionId": sessionId},
    );
  }
}
