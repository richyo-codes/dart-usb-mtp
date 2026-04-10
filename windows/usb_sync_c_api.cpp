#include "include/usb_sync/usb_sync_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "usb_sync_plugin.h"

void UsbSyncCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  usb_sync::UsbSyncPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
