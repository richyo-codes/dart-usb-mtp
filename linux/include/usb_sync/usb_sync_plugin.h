#ifndef FLUTTER_PLUGIN_USB_SYNC_PLUGIN_H_
#define FLUTTER_PLUGIN_USB_SYNC_PLUGIN_H_

#include <flutter_linux/flutter_linux.h>

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(UsbSyncPlugin, usb_sync_plugin, USB_SYNC, PLUGIN, GObject)

void usb_sync_plugin_register_with_registrar(FlPluginRegistrar* registrar);

G_END_DECLS

#endif  // FLUTTER_PLUGIN_USB_SYNC_PLUGIN_H_
