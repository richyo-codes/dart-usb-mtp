#include "include/usb_mtp_client/usb_sync_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "usb_mtp_client_plugin.h"

void UsbSyncCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  usb_sync::UsbSyncPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
