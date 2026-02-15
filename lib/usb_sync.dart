export "src/models.dart";
export "src/usb_sync_exception.dart";

import "dart:typed_data";

import "src/models.dart";
import "src/usb_sync_platform_interface.dart";

class UsbSync {
  const UsbSync._();

  static Future<List<UsbDeviceInfo>> listDevices({
    UsbFilter filter = const UsbFilter(),
  }) {
    return UsbSyncPlatform.instance.listDevices(filter: filter);
  }

  static Stream<UsbDeviceEvent> watchDevices({
    UsbFilter filter = const UsbFilter(),
  }) {
    return UsbSyncPlatform.instance.watchDevices(filter: filter);
  }

  static Future<UsbSession> openDevice({required String deviceId}) {
    return UsbSyncPlatform.instance.openDevice(deviceId: deviceId);
  }

  static Future<List<UsbEntry>> listEntries({
    required String sessionId,
    String path = "/",
  }) {
    return UsbSyncPlatform.instance.listEntries(
      sessionId: sessionId,
      path: path,
    );
  }

  static Future<Uint8List> readBytes({
    required String sessionId,
    required String path,
  }) {
    return UsbSyncPlatform.instance.readBytes(
      sessionId: sessionId,
      path: path,
    );
  }

  static Future<void> closeSession({required String sessionId}) {
    return UsbSyncPlatform.instance.closeSession(sessionId: sessionId);
  }
}
