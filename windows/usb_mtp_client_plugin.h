#ifndef FLUTTER_PLUGIN_USB_SYNC_PLUGIN_H_
#define FLUTTER_PLUGIN_USB_SYNC_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>

namespace usb_sync {

class UsbSyncPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  UsbSyncPlugin();
  ~UsbSyncPlugin() override;

  UsbSyncPlugin(const UsbSyncPlugin&) = delete;
  UsbSyncPlugin& operator=(const UsbSyncPlugin&) = delete;

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

 private:
  bool com_initialized_ = false;
};

}  // namespace usb_sync

#endif  // FLUTTER_PLUGIN_USB_SYNC_PLUGIN_H_
