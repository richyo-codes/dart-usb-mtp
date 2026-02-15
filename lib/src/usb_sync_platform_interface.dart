import "dart:typed_data";

import "package:plugin_platform_interface/plugin_platform_interface.dart";

import "models.dart";
import "usb_sync_method_channel.dart";

abstract class UsbSyncPlatform extends PlatformInterface {
  UsbSyncPlatform() : super(token: _token);

  static final Object _token = Object();

  static UsbSyncPlatform _instance = MethodChannelUsbSync();

  static UsbSyncPlatform get instance => _instance;

  static set instance(UsbSyncPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  Future<List<UsbDeviceInfo>> listDevices({
    UsbFilter filter = const UsbFilter(),
  });

  Stream<UsbDeviceEvent> watchDevices({
    UsbFilter filter = const UsbFilter(),
  });

  Future<UsbSession> openDevice({
    required String deviceId,
  });

  Future<List<UsbEntry>> listEntries({
    required String sessionId,
    String path = "/",
  });

  Future<Uint8List> readBytes({
    required String sessionId,
    required String path,
  });

  Future<void> closeSession({
    required String sessionId,
  });
}
