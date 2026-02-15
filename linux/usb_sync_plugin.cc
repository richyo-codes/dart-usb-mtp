#include "include/usb_sync/usb_sync_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <cstring>

struct _UsbSyncPlugin {
  GObject parent_instance;
};

G_DEFINE_TYPE(UsbSyncPlugin, usb_sync_plugin, g_object_get_type())

static void usb_sync_plugin_handle_method_call(
    UsbSyncPlugin* self,
    FlMethodCall* method_call) {
  const gchar* method = fl_method_call_get_name(method_call);
  g_autoptr(FlMethodResponse) response = nullptr;

  if (strcmp(method, "listDevices") == 0 || strcmp(method, "listEntries") == 0) {
    g_autoptr(FlValue) result = fl_value_new_list();
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  } else if (strcmp(method, "closeSession") == 0) {
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else {
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "unimplemented", "Method is not implemented for Linux yet.", nullptr));
  }

  fl_method_call_respond(method_call, response, nullptr);
}

static void usb_sync_plugin_dispose(GObject* object) {
  G_OBJECT_CLASS(usb_sync_plugin_parent_class)->dispose(object);
}

static void usb_sync_plugin_class_init(UsbSyncPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = usb_sync_plugin_dispose;
}

static void usb_sync_plugin_init(UsbSyncPlugin* self) {}

static void method_call_cb(
    FlMethodChannel* channel,
    FlMethodCall* method_call,
    gpointer user_data) {
  UsbSyncPlugin* plugin = USB_SYNC_PLUGIN(user_data);
  usb_sync_plugin_handle_method_call(plugin, method_call);
}

void usb_sync_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  UsbSyncPlugin* plugin = USB_SYNC_PLUGIN(
      g_object_new(usb_sync_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "usb_sync",
                            FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  g_object_unref(plugin);
}
