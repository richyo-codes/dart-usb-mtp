#include "include/usb_sync/usb_sync_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>

namespace {

constexpr const char* kTransportMtp = "mtp";
constexpr const char* kTransportMassStorage = "massStorage";
constexpr const char* kTransportUnknown = "unknown";

constexpr const char* kCapabilityEnumerate = "enumerate";
constexpr const char* kCapabilityReadFiles = "readFiles";
constexpr const char* kCapabilityMount = "mount";

struct DeviceFilter {
  std::set<int> vendor_ids;
  std::set<int> product_ids;
  std::set<std::string> transports;
  std::set<std::string> required_capabilities;
  std::string product_name_pattern;
  std::string manufacturer_pattern;
};

std::string ToLower(const std::string& value) {
  std::string output = value;
  std::transform(output.begin(), output.end(), output.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return output;
}

std::string Trim(const std::string& value) {
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    start++;
  }

  size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    end--;
  }
  return value.substr(start, end - start);
}

bool ReadTrimmedFile(const std::filesystem::path& path, std::string* out) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }

  std::string line;
  if (!std::getline(file, line)) {
    return false;
  }
  *out = Trim(line);
  return true;
}

bool ParseHexInt(const std::string& value, int* out) {
  if (value.empty()) {
    return false;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 16);
  if (end == value.c_str()) {
    return false;
  }
  *out = static_cast<int>(parsed);
  return true;
}

bool PatternMatches(const std::string& value, const std::string& pattern) {
  if (pattern.empty()) {
    return true;
  }
  if (value.empty()) {
    return false;
  }

  try {
    const std::regex regex(pattern, std::regex_constants::icase);
    return std::regex_search(value, regex);
  } catch (...) {
    const std::string lowered_value = ToLower(value);
    const std::string lowered_pattern = ToLower(pattern);
    return lowered_value.find(lowered_pattern) != std::string::npos;
  }
}

std::set<int> ParseIntSet(FlValue* value) {
  std::set<int> output;
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_LIST) {
    return output;
  }

  const size_t length = fl_value_get_length(value);
  for (size_t i = 0; i < length; i++) {
    FlValue* item = fl_value_get_list_value(value, i);
    if (item == nullptr) {
      continue;
    }
    const auto type = fl_value_get_type(item);
    if (type == FL_VALUE_TYPE_INT) {
      output.insert(static_cast<int>(fl_value_get_int(item)));
    } else if (type == FL_VALUE_TYPE_FLOAT) {
      output.insert(static_cast<int>(fl_value_get_float(item)));
    }
  }
  return output;
}

std::set<std::string> ParseStringSet(FlValue* value) {
  std::set<std::string> output;
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_LIST) {
    return output;
  }

  const size_t length = fl_value_get_length(value);
  for (size_t i = 0; i < length; i++) {
    FlValue* item = fl_value_get_list_value(value, i);
    if (item == nullptr || fl_value_get_type(item) != FL_VALUE_TYPE_STRING) {
      continue;
    }
    output.insert(fl_value_get_string(item));
  }
  return output;
}

std::string ParseString(FlValue* map, const char* key) {
  if (map == nullptr || fl_value_get_type(map) != FL_VALUE_TYPE_MAP) {
    return "";
  }
  FlValue* value = fl_value_lookup_string(map, key);
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_STRING) {
    return "";
  }
  return fl_value_get_string(value);
}

DeviceFilter ParseFilter(FlValue* args) {
  DeviceFilter filter;
  if (args == nullptr || fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
    return filter;
  }

  filter.vendor_ids = ParseIntSet(fl_value_lookup_string(args, "vendorIds"));
  filter.product_ids = ParseIntSet(fl_value_lookup_string(args, "productIds"));
  filter.transports = ParseStringSet(fl_value_lookup_string(args, "transports"));
  filter.required_capabilities =
      ParseStringSet(fl_value_lookup_string(args, "requiredCapabilities"));
  filter.product_name_pattern = ParseString(args, "productNamePattern");
  filter.manufacturer_pattern = ParseString(args, "manufacturerPattern");
  return filter;
}

std::set<std::string> DetectTransports(const std::filesystem::path& device_path,
                                       const std::string& device_name) {
  std::set<std::string> transports;

  std::string device_class_raw;
  if (ReadTrimmedFile(device_path / "bDeviceClass", &device_class_raw)) {
    int device_class = 0;
    if (ParseHexInt(device_class_raw, &device_class)) {
      if (device_class == 0x06) {
        transports.insert(kTransportMtp);
      } else if (device_class == 0x08) {
        transports.insert(kTransportMassStorage);
      }
    }
  }

  std::error_code ec;
  for (const auto& child : std::filesystem::directory_iterator(device_path, ec)) {
    if (ec) {
      break;
    }
    const std::string child_name = child.path().filename().string();
    if (child_name.rfind(device_name + ":", 0) != 0) {
      continue;
    }

    std::string interface_class_raw;
    if (!ReadTrimmedFile(child.path() / "bInterfaceClass", &interface_class_raw)) {
      continue;
    }

    int interface_class = 0;
    if (!ParseHexInt(interface_class_raw, &interface_class)) {
      continue;
    }
    if (interface_class == 0x06) {
      transports.insert(kTransportMtp);
    } else if (interface_class == 0x08) {
      transports.insert(kTransportMassStorage);
    }
  }

  if (transports.empty()) {
    transports.insert(kTransportUnknown);
  }
  return transports;
}

std::set<std::string> DetectCapabilities(
    const std::set<std::string>& transports) {
  std::set<std::string> capabilities{kCapabilityEnumerate};
  if (transports.count(kTransportMtp) > 0 ||
      transports.count(kTransportMassStorage) > 0) {
    capabilities.insert(kCapabilityReadFiles);
  }
  if (transports.count(kTransportMassStorage) > 0) {
    capabilities.insert(kCapabilityMount);
  }
  return capabilities;
}

bool MatchesSetIntersection(const std::set<std::string>& required,
                            const std::set<std::string>& actual) {
  if (required.empty()) {
    return true;
  }
  for (const auto& value : actual) {
    if (required.count(value) > 0) {
      return true;
    }
  }
  return false;
}

bool MatchesRequiredCapabilities(const std::set<std::string>& required,
                                 const std::set<std::string>& actual) {
  for (const auto& value : required) {
    if (actual.count(value) == 0) {
      return false;
    }
  }
  return true;
}

bool MatchesFilter(const DeviceFilter& filter,
                   int vendor_id,
                   int product_id,
                   const std::string& manufacturer_name,
                   const std::string& product_name,
                   const std::set<std::string>& transports,
                   const std::set<std::string>& capabilities) {
  if (!filter.vendor_ids.empty() && filter.vendor_ids.count(vendor_id) == 0) {
    return false;
  }
  if (!filter.product_ids.empty() && filter.product_ids.count(product_id) == 0) {
    return false;
  }
  if (!MatchesSetIntersection(filter.transports, transports)) {
    return false;
  }
  if (!MatchesRequiredCapabilities(filter.required_capabilities, capabilities)) {
    return false;
  }
  if (!PatternMatches(product_name, filter.product_name_pattern)) {
    return false;
  }
  if (!PatternMatches(manufacturer_name, filter.manufacturer_pattern)) {
    return false;
  }
  return true;
}

FlValue* NewStringList(const std::set<std::string>& values) {
  FlValue* list = fl_value_new_list();
  for (const auto& value : values) {
    fl_value_append_take(list, fl_value_new_string(value.c_str()));
  }
  return list;
}

FlValue* BuildDeviceList(const DeviceFilter& filter) {
  FlValue* list = fl_value_new_list();

  const std::filesystem::path root("/sys/bus/usb/devices");
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) {
      break;
    }

    const std::filesystem::path device_path = entry.path();
    const std::string device_name = device_path.filename().string();

    std::string vendor_raw;
    std::string product_raw;
    if (!ReadTrimmedFile(device_path / "idVendor", &vendor_raw) ||
        !ReadTrimmedFile(device_path / "idProduct", &product_raw)) {
      continue;
    }

    int vendor_id = 0;
    int product_id = 0;
    if (!ParseHexInt(vendor_raw, &vendor_id) ||
        !ParseHexInt(product_raw, &product_id)) {
      continue;
    }

    std::string manufacturer_name;
    std::string product_name;
    std::string serial_number;
    ReadTrimmedFile(device_path / "manufacturer", &manufacturer_name);
    ReadTrimmedFile(device_path / "product", &product_name);
    ReadTrimmedFile(device_path / "serial", &serial_number);

    const std::set<std::string> transports =
        DetectTransports(device_path, device_name);
    const std::set<std::string> capabilities = DetectCapabilities(transports);

    if (!MatchesFilter(filter, vendor_id, product_id, manufacturer_name,
                       product_name, transports, capabilities)) {
      continue;
    }

    FlValue* map = fl_value_new_map();
    fl_value_set_string_take(map, "id", fl_value_new_string(device_name.c_str()));
    fl_value_set_string_take(map, "vendorId", fl_value_new_int(vendor_id));
    fl_value_set_string_take(map, "productId", fl_value_new_int(product_id));
    if (!manufacturer_name.empty()) {
      fl_value_set_string_take(map, "manufacturerName",
                               fl_value_new_string(manufacturer_name.c_str()));
    }
    if (!product_name.empty()) {
      fl_value_set_string_take(map, "productName",
                               fl_value_new_string(product_name.c_str()));
    }
    if (!serial_number.empty()) {
      fl_value_set_string_take(map, "serialNumber",
                               fl_value_new_string(serial_number.c_str()));
    }
    fl_value_set_string_take(map, "transports", NewStringList(transports));
    fl_value_set_string_take(map, "capabilities", NewStringList(capabilities));
    fl_value_set_string_take(map, "isMounted", fl_value_new_null());
    fl_value_set_string_take(map, "mountPath", fl_value_new_null());

    fl_value_append_take(list, map);
  }

  return list;
}

}  // namespace

struct _UsbSyncPlugin {
  GObject parent_instance;
};

G_DEFINE_TYPE(UsbSyncPlugin, usb_sync_plugin, g_object_get_type())

static void usb_sync_plugin_handle_method_call(
    UsbSyncPlugin* /*self*/,
    FlMethodCall* method_call) {
  const gchar* method = fl_method_call_get_name(method_call);
  g_autoptr(FlMethodResponse) response = nullptr;

  if (strcmp(method, "listDevices") == 0) {
    FlValue* args = fl_method_call_get_args(method_call);
    const DeviceFilter filter = ParseFilter(args);
    g_autoptr(FlValue) result = BuildDeviceList(filter);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  } else if (strcmp(method, "listEntries") == 0) {
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
