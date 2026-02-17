#include "include/usb_sync/usb_sync_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

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

struct UsbDeviceRecord {
  std::string id;
  int vendor_id = 0;
  int product_id = 0;
  std::string manufacturer_name;
  std::string product_name;
  std::string serial_number;
  std::set<std::string> transports;
  std::set<std::string> capabilities;
  int bus_num = 0;
  int dev_num = 0;
  bool is_mounted = false;
  std::string mount_path;
};

std::map<std::string, std::filesystem::path> g_sessions;
uint64_t g_next_session_id = 1;

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

bool ParseDecInt(const std::string& value, int* out) {
  if (value.empty()) {
    return false;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
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

bool HasGioCommand() {
  static bool initialized = false;
  static bool available = false;
  if (!initialized) {
    gchar* path = g_find_program_in_path("gio");
    if (path != nullptr) {
      available = true;
      g_free(path);
    }
    initialized = true;
  }
  return available;
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
  if (transports.count(kTransportMassStorage) > 0 ||
      (transports.count(kTransportMtp) > 0 && HasGioCommand())) {
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

std::string UrlDecode(const std::string& raw) {
  std::string decoded;
  decoded.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i] == '%' && i + 2 < raw.size()) {
      const std::string hex = raw.substr(i + 1, 2);
      char* end = nullptr;
      const long value = std::strtol(hex.c_str(), &end, 16);
      if (end != hex.c_str()) {
        decoded.push_back(static_cast<char>(value));
        i += 2;
        continue;
      }
    }
    decoded.push_back(raw[i]);
  }
  return decoded;
}

std::string NormalizeSerial(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) != 0) {
      out.push_back(static_cast<char>(std::tolower(uc)));
    }
  }
  return out;
}

std::string StripLeadingZeros(const std::string& raw) {
  size_t start = 0;
  while (start < raw.size() && raw[start] == '0') {
    start++;
  }
  return raw.substr(start);
}

std::string Pad3(int value) {
  std::ostringstream stream;
  stream << std::setw(3) << std::setfill('0') << value;
  return stream.str();
}

std::filesystem::path GvfsMountRoot() {
  return std::filesystem::path("/run/user") /
         std::to_string(static_cast<uint64_t>(getuid())) / "gvfs";
}

std::optional<std::filesystem::path> FindMtpMountPath(const UsbDeviceRecord& device) {
  const std::filesystem::path root = GvfsMountRoot();
  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec) {
    return std::nullopt;
  }

  const bool can_match_busdev = device.bus_num > 0 && device.dev_num > 0;
  const std::string bus = can_match_busdev ? Pad3(device.bus_num) : "";
  const std::string dev = can_match_busdev ? Pad3(device.dev_num) : "";
  const std::string normalized_serial = NormalizeSerial(device.serial_number);

  const std::regex usb_pattern("usb:([0-9]{3}),([0-9]{3})");
  const std::regex vidpid_pattern(
      "mtp:host=([0-9a-fA-F]{4})_([0-9a-fA-F]{4})(?:_([^/]+))?");
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_directory(ec) || ec) {
      continue;
    }

    const std::string mount_name = entry.path().filename().string();
    if (mount_name.rfind("mtp:host=", 0) != 0) {
      continue;
    }

    const std::string decoded = UrlDecode(mount_name);
    bool matched = false;

    if (can_match_busdev) {
      std::smatch usb_match;
      if (std::regex_search(decoded, usb_match, usb_pattern) &&
          usb_match.size() >= 3 &&
          usb_match[1].str() == bus &&
          usb_match[2].str() == dev) {
        matched = true;
      }
    }

    if (!matched && device.vendor_id > 0 && device.product_id > 0) {
      std::smatch vp_match;
      if (std::regex_search(decoded, vp_match, vidpid_pattern) &&
          vp_match.size() >= 3) {
        int mount_vid = 0;
        int mount_pid = 0;
        if (ParseHexInt(vp_match[1].str(), &mount_vid) &&
            ParseHexInt(vp_match[2].str(), &mount_pid) &&
            mount_vid == device.vendor_id &&
            mount_pid == device.product_id) {
          if (vp_match.size() >= 4 && !vp_match[3].str().empty() &&
              !normalized_serial.empty()) {
            const std::string mount_serial = NormalizeSerial(vp_match[3].str());
            const std::string lhs = StripLeadingZeros(normalized_serial);
            const std::string rhs = StripLeadingZeros(mount_serial);
            matched = mount_serial == normalized_serial || lhs == rhs ||
                      normalized_serial.find(mount_serial) != std::string::npos ||
                      mount_serial.find(normalized_serial) != std::string::npos;
          } else {
            matched = true;
          }
        }
      }
    }

    if (matched) {
      return entry.path();
    }
  }
  return std::nullopt;
}

bool RunCommand(const std::vector<std::string>& args, std::string* error_out) {
  if (args.empty()) {
    if (error_out != nullptr) {
      *error_out = "No command provided.";
    }
    return false;
  }

  std::vector<gchar*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) {
    argv.push_back(const_cast<gchar*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  gint exit_status = 0;
  gchar* std_out = nullptr;
  gchar* std_err = nullptr;
  g_autoptr(GError) error = nullptr;
  const gboolean spawned = g_spawn_sync(
      nullptr,
      argv.data(),
      nullptr,
      G_SPAWN_SEARCH_PATH,
      nullptr,
      nullptr,
      &std_out,
      &std_err,
      &exit_status,
      &error);
  g_autofree gchar* scoped_out = std_out;
  g_autofree gchar* scoped_err = std_err;

  if (!spawned) {
    if (error_out != nullptr) {
      *error_out =
          error != nullptr ? error->message : "Failed to start child process.";
    }
    return false;
  }

  g_autoptr(GError) wait_error = nullptr;
  if (!g_spawn_check_wait_status(exit_status, &wait_error)) {
    if (error_out != nullptr) {
      const std::string stderr_text =
          std_err != nullptr ? std::string(std_err) : std::string();
      if (!stderr_text.empty()) {
        *error_out = stderr_text;
      } else if (wait_error != nullptr) {
        *error_out = wait_error->message;
      } else {
        *error_out = "Command exited with a non-zero status.";
      }
    }
    return false;
  }

  return true;
}

bool TryMountMtpViaGio(int bus_num, int dev_num, std::string* error_out) {
  if (bus_num <= 0 || dev_num <= 0) {
    if (error_out != nullptr) {
      *error_out = "Missing bus/dev number.";
    }
    return false;
  }
  if (!HasGioCommand()) {
    if (error_out != nullptr) {
      *error_out = "gio executable not found in PATH.";
    }
    return false;
  }

  const std::string bus = Pad3(bus_num);
  const std::string dev = Pad3(dev_num);
  const std::string uri = "mtp://[usb:" + bus + "," + dev + "]/";

  std::string primary_error;
  if (RunCommand({"gio", "mount", uri}, &primary_error)) {
    return true;
  }

  const std::string node = "/dev/bus/usb/" + bus + "/" + dev;
  std::string fallback_error;
  if (RunCommand({"gio", "mount", "-d", node}, &fallback_error)) {
    return true;
  }

  if (error_out != nullptr) {
    *error_out = "gio mount failed for URI '" + uri + "'. " + primary_error +
                 " Fallback error: " + fallback_error;
  }
  return false;
}

std::optional<std::filesystem::path> EnsureMtpMountPath(
    const UsbDeviceRecord& device,
    bool allow_mount,
    std::string* error_out) {
  if (device.bus_num <= 0 || device.dev_num <= 0) {
    if (error_out != nullptr) {
      *error_out = "Missing USB bus/dev values.";
    }
    return std::nullopt;
  }

  auto existing = FindMtpMountPath(device);
  if (existing.has_value()) {
    return existing;
  }
  if (!allow_mount) {
    return std::nullopt;
  }

  std::string mount_error;
  if (!TryMountMtpViaGio(device.bus_num, device.dev_num, &mount_error)) {
    if (error_out != nullptr) {
      *error_out = mount_error;
    }
    return std::nullopt;
  }

  for (int i = 0; i < 15; i++) {
    g_usleep(200000);
    auto mounted = FindMtpMountPath(device);
    if (mounted.has_value()) {
      return mounted;
    }
  }

  if (error_out != nullptr) {
    *error_out = "MTP mount did not appear in GVFS.";
  }
  return std::nullopt;
}

std::vector<UsbDeviceRecord> EnumerateDevices() {
  std::vector<UsbDeviceRecord> devices;
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

    int bus_num = 0;
    int dev_num = 0;
    std::string bus_raw;
    std::string dev_raw;
    if (ReadTrimmedFile(device_path / "busnum", &bus_raw)) {
      ParseDecInt(bus_raw, &bus_num);
    }
    if (ReadTrimmedFile(device_path / "devnum", &dev_raw)) {
      ParseDecInt(dev_raw, &dev_num);
    }

    std::set<std::string> transports =
        DetectTransports(device_path, device_name);
    std::set<std::string> capabilities = DetectCapabilities(transports);

    bool is_mounted = false;
    std::string mount_path;
    UsbDeviceRecord probe;
    probe.vendor_id = vendor_id;
    probe.product_id = product_id;
    probe.serial_number = serial_number;
    probe.bus_num = bus_num;
    probe.dev_num = dev_num;
    auto existing_mount = FindMtpMountPath(probe);
    if (existing_mount.has_value()) {
      is_mounted = true;
      mount_path = existing_mount->string();
      transports.insert(kTransportMtp);
      capabilities.insert(kCapabilityReadFiles);
      capabilities.insert(kCapabilityMount);
    }

    UsbDeviceRecord record;
    record.id = device_name;
    record.vendor_id = vendor_id;
    record.product_id = product_id;
    record.manufacturer_name = manufacturer_name;
    record.product_name = product_name;
    record.serial_number = serial_number;
    record.transports = transports;
    record.capabilities = capabilities;
    record.bus_num = bus_num;
    record.dev_num = dev_num;
    record.is_mounted = is_mounted;
    record.mount_path = mount_path;
    devices.push_back(std::move(record));
  }
  return devices;
}

std::optional<UsbDeviceRecord> FindDeviceById(const std::string& device_id) {
  for (const auto& device : EnumerateDevices()) {
    if (device.id == device_id) {
      return device;
    }
  }
  return std::nullopt;
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

  for (const auto& device : EnumerateDevices()) {
    if (!MatchesFilter(filter,
                       device.vendor_id,
                       device.product_id,
                       device.manufacturer_name,
                       device.product_name,
                       device.transports,
                       device.capabilities)) {
      continue;
    }

    FlValue* map = fl_value_new_map();
    fl_value_set_string_take(map, "id", fl_value_new_string(device.id.c_str()));
    fl_value_set_string_take(map, "vendorId", fl_value_new_int(device.vendor_id));
    fl_value_set_string_take(map, "productId", fl_value_new_int(device.product_id));
    if (!device.manufacturer_name.empty()) {
      fl_value_set_string_take(map, "manufacturerName",
                               fl_value_new_string(device.manufacturer_name.c_str()));
    }
    if (!device.product_name.empty()) {
      fl_value_set_string_take(map, "productName",
                               fl_value_new_string(device.product_name.c_str()));
    }
    if (!device.serial_number.empty()) {
      fl_value_set_string_take(map, "serialNumber",
                               fl_value_new_string(device.serial_number.c_str()));
    }
    fl_value_set_string_take(map, "transports", NewStringList(device.transports));
    fl_value_set_string_take(map, "capabilities", NewStringList(device.capabilities));
    fl_value_set_string_take(map, "isMounted", fl_value_new_bool(device.is_mounted));
    if (!device.mount_path.empty()) {
      fl_value_set_string_take(map, "mountPath",
                               fl_value_new_string(device.mount_path.c_str()));
    } else {
      fl_value_set_string_take(map, "mountPath", fl_value_new_null());
    }

    fl_value_append_take(list, map);
  }

  return list;
}

std::string NextSessionId(const std::string& device_id) {
  return "linux:" + device_id + ":" + std::to_string(g_next_session_id++);
}

bool IsPathInsideRoot(const std::filesystem::path& root,
                      const std::filesystem::path& candidate) {
  const auto normalized_root = root.lexically_normal();
  const auto normalized_candidate = candidate.lexically_normal();
  auto root_it = normalized_root.begin();
  auto cand_it = normalized_candidate.begin();
  for (; root_it != normalized_root.end(); ++root_it, ++cand_it) {
    if (cand_it == normalized_candidate.end() || *root_it != *cand_it) {
      return false;
    }
  }
  return true;
}

bool ResolveSessionPath(const std::filesystem::path& session_root,
                        const std::string& requested_path,
                        std::filesystem::path* out_path,
                        std::string* error_out) {
  if (out_path == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Output path pointer is null.";
    }
    return false;
  }

  std::filesystem::path relative =
      requested_path.empty() ? std::filesystem::path("/") :
                               std::filesystem::path(requested_path);
  if (relative.is_absolute()) {
    relative = relative.lexically_relative("/");
  }

  const std::filesystem::path resolved = (session_root / relative).lexically_normal();
  if (!IsPathInsideRoot(session_root, resolved)) {
    if (error_out != nullptr) {
      *error_out = "Path escapes session root.";
    }
    return false;
  }

  *out_path = resolved;
  return true;
}

int64_t ToEpochMs(const std::filesystem::file_time_type& time_value) {
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  using std::chrono::system_clock;
  const auto adjusted = system_clock::now() +
                        (time_value - std::filesystem::file_time_type::clock::now());
  return duration_cast<milliseconds>(adjusted.time_since_epoch()).count();
}

FlValue* BuildEntriesList(const std::filesystem::path& session_root,
                          const std::filesystem::path& directory) {
  struct EntryRow {
    std::string path;
    std::string name;
    bool is_directory = false;
    int64_t size_bytes = -1;
    int64_t modified_at_ms = -1;
  };

  std::vector<EntryRow> rows;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (ec) {
      break;
    }

    EntryRow row;
    row.name = entry.path().filename().string();

    const std::filesystem::path rel = entry.path().lexically_relative(session_root);
    const std::string rel_str = rel.generic_string();
    row.path = "/" + rel_str;
    row.is_directory = entry.is_directory(ec);
    if (ec) {
      ec.clear();
      row.is_directory = false;
    }

    if (!row.is_directory && entry.is_regular_file(ec) && !ec) {
      row.size_bytes = static_cast<int64_t>(entry.file_size(ec));
      if (ec) {
        row.size_bytes = -1;
        ec.clear();
      }
    } else {
      ec.clear();
    }

    const auto write_time = entry.last_write_time(ec);
    if (!ec) {
      row.modified_at_ms = ToEpochMs(write_time);
    } else {
      ec.clear();
    }

    rows.push_back(row);
  }

  std::sort(rows.begin(), rows.end(), [](const EntryRow& a, const EntryRow& b) {
    if (a.is_directory != b.is_directory) {
      return a.is_directory > b.is_directory;
    }
    return a.name < b.name;
  });

  FlValue* list = fl_value_new_list();
  for (const auto& row : rows) {
    FlValue* map = fl_value_new_map();
    fl_value_set_string_take(map, "path", fl_value_new_string(row.path.c_str()));
    fl_value_set_string_take(map, "name", fl_value_new_string(row.name.c_str()));
    fl_value_set_string_take(map, "isDirectory", fl_value_new_bool(row.is_directory));
    if (row.size_bytes >= 0) {
      fl_value_set_string_take(map, "sizeBytes", fl_value_new_int(row.size_bytes));
    } else {
      fl_value_set_string_take(map, "sizeBytes", fl_value_new_null());
    }
    if (row.modified_at_ms >= 0) {
      fl_value_set_string_take(map, "modifiedAtMs",
                               fl_value_new_int(row.modified_at_ms));
    } else {
      fl_value_set_string_take(map, "modifiedAtMs", fl_value_new_null());
    }
    fl_value_append_take(list, map);
  }
  return list;
}

bool ReadFileBytes(const std::filesystem::path& file_path,
                   std::vector<uint8_t>* out_bytes,
                   std::string* error_out) {
  if (out_bytes == nullptr) {
    if (error_out != nullptr) {
      *error_out = "Output buffer pointer is null.";
    }
    return false;
  }

  std::ifstream file(file_path, std::ios::binary);
  if (!file.is_open()) {
    if (error_out != nullptr) {
      *error_out = "Unable to open file for reading.";
    }
    return false;
  }

  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
  *out_bytes = bytes;
  return true;
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
  } else if (strcmp(method, "openDevice") == 0) {
    FlValue* args = fl_method_call_get_args(method_call);
    const std::string device_id = ParseString(args, "deviceId");
    if (device_id.empty()) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new(
          "invalid_args", "Missing required argument: deviceId.", nullptr));
    } else {
      const std::optional<UsbDeviceRecord> maybe_device = FindDeviceById(device_id);
      if (!maybe_device.has_value()) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new(
            "not_found", "Requested USB device was not found.", nullptr));
      } else {
        std::filesystem::path session_root;
        std::string mount_error;
        if (maybe_device->transports.count(kTransportMtp) > 0) {
          const auto mount_path =
              EnsureMtpMountPath(*maybe_device, true, &mount_error);
          if (!mount_path.has_value()) {
            response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                "mtp_mount_failed", mount_error.c_str(), nullptr));
          } else {
            session_root = *mount_path;
          }
        } else if (maybe_device->is_mounted && !maybe_device->mount_path.empty()) {
          session_root = maybe_device->mount_path;
        } else {
          response = FL_METHOD_RESPONSE(fl_method_error_response_new(
              "unsupported_transport",
              "Device is not in a supported mounted mode.", nullptr));
        }

        if (response == nullptr) {
          std::error_code ec;
          if (!std::filesystem::exists(session_root, ec) || ec) {
            response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                "mount_not_accessible",
                "Mount path does not exist or is not accessible.", nullptr));
          } else {
            const std::string session_id = NextSessionId(maybe_device->id);
            g_sessions[session_id] = session_root;

            g_autoptr(FlValue) result = fl_value_new_map();
            fl_value_set_string_take(
                result, "sessionId", fl_value_new_string(session_id.c_str()));
            fl_value_set_string_take(
                result, "deviceId", fl_value_new_string(maybe_device->id.c_str()));
            response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
          }
        }
      }
    }
  } else if (strcmp(method, "listEntries") == 0) {
    FlValue* args = fl_method_call_get_args(method_call);
    const std::string session_id = ParseString(args, "sessionId");
    std::string requested_path = ParseString(args, "path");
    if (requested_path.empty()) {
      requested_path = "/";
    }
    if (session_id.empty()) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new(
          "invalid_args", "Missing required argument: sessionId.", nullptr));
    } else {
      const auto session_it = g_sessions.find(session_id);
      if (session_it == g_sessions.end()) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new(
            "session_not_found", "Session does not exist or is closed.", nullptr));
      } else {
        std::filesystem::path directory_path;
        std::string resolve_error;
        if (!ResolveSessionPath(session_it->second,
                                requested_path,
                                &directory_path,
                                &resolve_error)) {
          response = FL_METHOD_RESPONSE(fl_method_error_response_new(
              "invalid_path", resolve_error.c_str(), nullptr));
        } else {
          std::error_code ec;
          if (!std::filesystem::exists(directory_path, ec) || ec) {
            response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                "path_not_found", "Requested path does not exist.", nullptr));
          } else if (!std::filesystem::is_directory(directory_path, ec) || ec) {
            response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                "not_directory", "Requested path is not a directory.", nullptr));
          } else {
            g_autoptr(FlValue) result =
                BuildEntriesList(session_it->second, directory_path);
            response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
          }
        }
      }
    }
  } else if (strcmp(method, "readBytes") == 0) {
    FlValue* args = fl_method_call_get_args(method_call);
    const std::string session_id = ParseString(args, "sessionId");
    const std::string requested_path = ParseString(args, "path");
    if (session_id.empty() || requested_path.empty()) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new(
          "invalid_args",
          "Missing required arguments: sessionId and path.",
          nullptr));
    } else {
      const auto session_it = g_sessions.find(session_id);
      if (session_it == g_sessions.end()) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new(
            "session_not_found", "Session does not exist or is closed.", nullptr));
      } else {
        std::filesystem::path file_path;
        std::string resolve_error;
        if (!ResolveSessionPath(session_it->second,
                                requested_path,
                                &file_path,
                                &resolve_error)) {
          response = FL_METHOD_RESPONSE(fl_method_error_response_new(
              "invalid_path", resolve_error.c_str(), nullptr));
        } else {
          std::error_code ec;
          if (!std::filesystem::exists(file_path, ec) || ec) {
            response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                "path_not_found", "Requested file does not exist.", nullptr));
          } else if (!std::filesystem::is_regular_file(file_path, ec) || ec) {
            response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                "not_file", "Requested path is not a regular file.", nullptr));
          } else {
            std::vector<uint8_t> bytes;
            std::string read_error;
            if (!ReadFileBytes(file_path, &bytes, &read_error)) {
              response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                  "read_failed", read_error.c_str(), nullptr));
            } else {
              g_autoptr(FlValue) result = fl_value_new_uint8_list(bytes.data(),
                                                                   bytes.size());
              response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
            }
          }
        }
      }
    }
  } else if (strcmp(method, "closeSession") == 0) {
    FlValue* args = fl_method_call_get_args(method_call);
    const std::string session_id = ParseString(args, "sessionId");
    if (session_id.empty()) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new(
          "invalid_args", "Missing required argument: sessionId.", nullptr));
    } else {
      g_sessions.erase(session_id);
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
    }
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
