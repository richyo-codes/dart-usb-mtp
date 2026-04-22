//
//  Generated file. Do not edit.
//

// clang-format off

#include "generated_plugin_registrant.h"

#include <usb_mtp_client/usb_sync_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) usb_mtp_client_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "UsbSyncPlugin");
  usb_sync_plugin_register_with_registrar(usb_mtp_client_registrar);
}
