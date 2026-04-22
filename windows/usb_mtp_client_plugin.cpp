#include "usb_mtp_client_plugin.h"

#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <portabledeviceapi.h>
#include <portabledevicetypes.h>
#include <propvarutil.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace usb_sync {
namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;
using Microsoft::WRL::ComPtr;

constexpr char kTransportMtp[] = "mtp";
constexpr char kCapabilityEnumerate[] = "enumerate";
constexpr char kCapabilityReadFiles[] = "readFiles";

struct DeviceFilter {
  std::set<int> vendor_ids;
  std::set<int> product_ids;
  std::set<std::string> transports;
  std::set<std::string> required_capabilities;
  std::string product_name_pattern;
  std::string manufacturer_pattern;
};

struct UsbDeviceRecord {
  std::wstring object_id_wide;
  std::string id;
  std::optional<int> vendor_id;
  std::optional<int> product_id;
  std::string manufacturer_name;
  std::string product_name;
  std::string serial_number;
  std::set<std::string> transports{kTransportMtp};
  std::set<std::string> capabilities{kCapabilityEnumerate, kCapabilityReadFiles};
};

struct StorageRoot {
  std::wstring object_id_wide;
  std::string label;
  std::string path;
};

struct ResolvedNode {
  std::wstring object_id_wide;
  std::string path;
  std::string name;
  bool is_directory = false;
  std::optional<int64_t> size_bytes;
  std::optional<int64_t> modified_at_ms;
};

struct SessionState {
  std::string session_id;
  std::string device_id;
  ComPtr<IPortableDevice> device;
  ComPtr<IPortableDeviceContent> content;
  std::vector<StorageRoot> storage_roots;
};

std::unordered_map<std::string, SessionState> g_sessions;
uint64_t g_next_session_ordinal = 1;

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return "";
  }
  const int size = WideCharToMultiByte(
      CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0,
      nullptr, nullptr);
  if (size <= 0) {
    return "";
  }
  std::string output(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                      output.data(), size, nullptr, nullptr);
  return output;
}

std::string ToLowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::wstring ToLowerAsciiWide(std::wstring value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return value;
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

std::string SanitizePathSegment(std::string value) {
  value = Trim(value);
  for (char& c : value) {
    if (c == '/' || c == '\\') {
      c = ' ';
    }
  }
  return Trim(value);
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
    return ToLowerAscii(value).find(ToLowerAscii(pattern)) != std::string::npos;
  }
}

std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> segments;
  std::string current;
  for (char c : path) {
    if (c == '/') {
      if (!current.empty()) {
        segments.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    segments.push_back(current);
  }
  return segments;
}

std::string NormalizePath(std::string path) {
  if (path.empty()) {
    return "/";
  }
  std::replace(path.begin(), path.end(), '\\', '/');
  std::string normalized;
  normalized.reserve(path.size() + 1);
  if (path.front() != '/') {
    normalized.push_back('/');
  }
  bool previous_was_slash = false;
  for (char c : path) {
    if (c == '/') {
      if (!previous_was_slash) {
        normalized.push_back('/');
      }
      previous_was_slash = true;
    } else {
      normalized.push_back(c);
      previous_was_slash = false;
    }
  }
  while (normalized.size() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }
  return normalized.empty() ? "/" : normalized;
}

const EncodableValue* FindMapValue(const EncodableMap& map,
                                   const char* key) {
  const auto it = map.find(EncodableValue(key));
  if (it == map.end()) {
    return nullptr;
  }
  return &it->second;
}

std::set<int> ParseIntSet(const EncodableValue* value) {
  std::set<int> output;
  const auto* list = value == nullptr ? nullptr : std::get_if<EncodableList>(value);
  if (list == nullptr) {
    return output;
  }

  for (const auto& item : *list) {
    if (const auto* int32_value = std::get_if<int32_t>(&item)) {
      output.insert(*int32_value);
    } else if (const auto* int64_value = std::get_if<int64_t>(&item)) {
      output.insert(static_cast<int>(*int64_value));
    }
  }
  return output;
}

std::set<std::string> ParseStringSet(const EncodableValue* value) {
  std::set<std::string> output;
  const auto* list = value == nullptr ? nullptr : std::get_if<EncodableList>(value);
  if (list == nullptr) {
    return output;
  }

  for (const auto& item : *list) {
    if (const auto* string_value = std::get_if<std::string>(&item)) {
      output.insert(*string_value);
    }
  }
  return output;
}

std::string ParseString(const EncodableValue* value) {
  const auto* string_value =
      value == nullptr ? nullptr : std::get_if<std::string>(value);
  return string_value == nullptr ? "" : *string_value;
}

DeviceFilter ParseFilter(const EncodableValue* value) {
  DeviceFilter filter;
  const auto* map = value == nullptr ? nullptr : std::get_if<EncodableMap>(value);
  if (map == nullptr) {
    return filter;
  }
  filter.vendor_ids = ParseIntSet(FindMapValue(*map, "vendorIds"));
  filter.product_ids = ParseIntSet(FindMapValue(*map, "productIds"));
  filter.transports = ParseStringSet(FindMapValue(*map, "transports"));
  filter.required_capabilities =
      ParseStringSet(FindMapValue(*map, "requiredCapabilities"));
  filter.product_name_pattern = ParseString(FindMapValue(*map, "productNamePattern"));
  filter.manufacturer_pattern =
      ParseString(FindMapValue(*map, "manufacturerPattern"));
  return filter;
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

bool MatchesFilter(const DeviceFilter& filter, const UsbDeviceRecord& device) {
  if (!filter.vendor_ids.empty() &&
      (!device.vendor_id.has_value() ||
       filter.vendor_ids.count(*device.vendor_id) == 0)) {
    return false;
  }
  if (!filter.product_ids.empty() &&
      (!device.product_id.has_value() ||
       filter.product_ids.count(*device.product_id) == 0)) {
    return false;
  }
  if (!MatchesSetIntersection(filter.transports, device.transports)) {
    return false;
  }
  if (!MatchesRequiredCapabilities(filter.required_capabilities,
                                   device.capabilities)) {
    return false;
  }
  if (!PatternMatches(device.product_name, filter.product_name_pattern)) {
    return false;
  }
  if (!PatternMatches(device.manufacturer_name, filter.manufacturer_pattern)) {
    return false;
  }
  return true;
}

std::optional<int> ParseHexSuffix(const std::wstring& input,
                                  const std::wstring& prefix) {
  const std::wstring lowered = ToLowerAsciiWide(input);
  const size_t start = lowered.find(prefix);
  if (start == std::wstring::npos) {
    return std::nullopt;
  }
  const size_t value_start = start + prefix.size();
  if (value_start + 4 > lowered.size()) {
    return std::nullopt;
  }
  const std::wstring raw = lowered.substr(value_start, 4);
  wchar_t* end = nullptr;
  const long parsed = std::wcstol(raw.c_str(), &end, 16);
  if (end == raw.c_str()) {
    return std::nullopt;
  }
  return static_cast<int>(parsed);
}

std::string DeriveSerial(const std::wstring& device_id) {
  const size_t hash_index = device_id.find_last_of(L'#');
  if (hash_index != std::wstring::npos && hash_index + 1 < device_id.size()) {
    return WideToUtf8(device_id.substr(hash_index + 1));
  }
  const size_t slash_index = device_id.find_last_of(L'\\');
  if (slash_index != std::wstring::npos && slash_index + 1 < device_id.size()) {
    return WideToUtf8(device_id.substr(slash_index + 1));
  }
  return "";
}

std::string NextSessionId(const std::string& device_id) {
  return "windows:" + device_id + ":" + std::to_string(g_next_session_ordinal++);
}

std::string HResultMessage(HRESULT hr, const std::string& fallback) {
  LPWSTR buffer = nullptr;
  const DWORD size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::string message = fallback;
  if (size > 0 && buffer != nullptr) {
    message = Trim(WideToUtf8(buffer));
  }
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  return message;
}

HRESULT CreatePortableDeviceManager(ComPtr<IPortableDeviceManager>* manager) {
  return CoCreateInstance(CLSID_PortableDeviceManager, nullptr,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(manager->ReleaseAndGetAddressOf()));
}

HRESULT CreateClientInfo(ComPtr<IPortableDeviceValues>* client_info) {
  ComPtr<IPortableDeviceValues> values;
  HRESULT hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&values));
  if (FAILED(hr)) {
    return hr;
  }
  values->SetStringValue(WPD_CLIENT_NAME, L"usb_mtp_client");
  values->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 0);
  values->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 1);
  values->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0);
  values->SetUnsignedIntegerValue(WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE,
                                  SECURITY_IMPERSONATION);
  *client_info = std::move(values);
  return S_OK;
}

HRESULT OpenPortableDevice(const std::wstring& device_id,
                           ComPtr<IPortableDevice>* device,
                           ComPtr<IPortableDeviceContent>* content) {
  ComPtr<IPortableDeviceValues> client_info;
  HRESULT hr = CreateClientInfo(&client_info);
  if (FAILED(hr)) {
    return hr;
  }

  hr = CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(device->ReleaseAndGetAddressOf()));
  if (FAILED(hr)) {
    return hr;
  }

  hr = (*device)->Open(device_id.c_str(), client_info.Get());
  if (FAILED(hr)) {
    return hr;
  }

  hr = (*device)->Content(content->ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    device->Reset();
    return hr;
  }
  return S_OK;
}

std::vector<std::wstring> EnumerateDeviceIds() {
  std::vector<std::wstring> ids;
  ComPtr<IPortableDeviceManager> manager;
  if (FAILED(CreatePortableDeviceManager(&manager))) {
    return ids;
  }

  DWORD count = 0;
  if (FAILED(manager->GetDevices(nullptr, &count)) || count == 0) {
    return ids;
  }

  std::vector<PWSTR> raw_ids(count, nullptr);
  if (FAILED(manager->GetDevices(raw_ids.data(), &count))) {
    for (PWSTR raw_id : raw_ids) {
      if (raw_id != nullptr) {
        CoTaskMemFree(raw_id);
      }
    }
    return ids;
  }

  ids.reserve(count);
  for (DWORD i = 0; i < count; i++) {
    if (raw_ids[i] != nullptr) {
      ids.emplace_back(raw_ids[i]);
      CoTaskMemFree(raw_ids[i]);
    }
  }
  return ids;
}

std::wstring ReadManagerString(
    const std::wstring& device_id,
    const std::function<HRESULT(IPortableDeviceManager*, LPCWSTR, LPWSTR,
                                DWORD*)>& getter) {
  ComPtr<IPortableDeviceManager> manager;
  if (FAILED(CreatePortableDeviceManager(&manager))) {
    return L"";
  }

  DWORD length = 0;
  HRESULT hr = getter(manager.Get(), device_id.c_str(), nullptr, &length);
  if (FAILED(hr) || length == 0) {
    return L"";
  }

  std::wstring buffer(length, L'\0');
  hr = getter(manager.Get(), device_id.c_str(), buffer.data(), &length);
  if (FAILED(hr)) {
    return L"";
  }
  while (!buffer.empty() && buffer.back() == L'\0') {
    buffer.pop_back();
  }
  return buffer;
}

std::vector<UsbDeviceRecord> EnumerateDevices() {
  std::vector<UsbDeviceRecord> devices;
  for (const auto& device_id_wide : EnumerateDeviceIds()) {
    UsbDeviceRecord record;
    record.object_id_wide = device_id_wide;
    record.id = WideToUtf8(device_id_wide);
    record.vendor_id = ParseHexSuffix(device_id_wide, L"vid_");
    record.product_id = ParseHexSuffix(device_id_wide, L"pid_");
    record.manufacturer_name = WideToUtf8(ReadManagerString(
        device_id_wide,
        [](IPortableDeviceManager* manager, LPCWSTR id, LPWSTR value,
           DWORD* length) {
          return manager->GetDeviceManufacturer(id, value, length);
        }));
    record.product_name = WideToUtf8(ReadManagerString(
        device_id_wide,
        [](IPortableDeviceManager* manager, LPCWSTR id, LPWSTR value,
           DWORD* length) {
          return manager->GetDeviceFriendlyName(id, value, length);
        }));
    if (record.product_name.empty()) {
      record.product_name = WideToUtf8(ReadManagerString(
          device_id_wide,
          [](IPortableDeviceManager* manager, LPCWSTR id, LPWSTR value,
             DWORD* length) {
            return manager->GetDeviceDescription(id, value, length);
          }));
    }
    record.serial_number = DeriveSerial(device_id_wide);
    devices.push_back(std::move(record));
  }
  std::sort(devices.begin(), devices.end(),
            [](const UsbDeviceRecord& a, const UsbDeviceRecord& b) {
              return ToLowerAscii(a.product_name + a.id) <
                     ToLowerAscii(b.product_name + b.id);
            });
  return devices;
}

EncodableList NewStringList(const std::set<std::string>& values) {
  EncodableList list;
  for (const auto& value : values) {
    list.emplace_back(value);
  }
  return list;
}

EncodableMap BuildDeviceMap(const UsbDeviceRecord& device) {
  EncodableMap map;
  map[EncodableValue("id")] = EncodableValue(device.id);
  map[EncodableValue("vendorId")] = device.vendor_id.has_value()
                                        ? EncodableValue(*device.vendor_id)
                                        : EncodableValue();
  map[EncodableValue("productId")] = device.product_id.has_value()
                                         ? EncodableValue(*device.product_id)
                                         : EncodableValue();
  map[EncodableValue("manufacturerName")] = device.manufacturer_name.empty()
                                                ? EncodableValue()
                                                : EncodableValue(device.manufacturer_name);
  map[EncodableValue("productName")] = device.product_name.empty()
                                           ? EncodableValue()
                                           : EncodableValue(device.product_name);
  map[EncodableValue("serialNumber")] = device.serial_number.empty()
                                            ? EncodableValue()
                                            : EncodableValue(device.serial_number);
  map[EncodableValue("transports")] = EncodableValue(NewStringList(device.transports));
  map[EncodableValue("capabilities")] =
      EncodableValue(NewStringList(device.capabilities));
  map[EncodableValue("isMounted")] = EncodableValue();
  map[EncodableValue("mountPath")] = EncodableValue();
  return map;
}

HRESULT CreatePropertiesKeyCollection(
    ComPtr<IPortableDeviceKeyCollection>* keys) {
  HRESULT hr = CoCreateInstance(CLSID_PortableDeviceKeyCollection, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(keys->ReleaseAndGetAddressOf()));
  if (FAILED(hr)) {
    return hr;
  }
  (*keys)->Add(WPD_OBJECT_NAME);
  (*keys)->Add(WPD_OBJECT_ORIGINAL_FILE_NAME);
  (*keys)->Add(WPD_STORAGE_DESCRIPTION);
  (*keys)->Add(WPD_OBJECT_SIZE);
  (*keys)->Add(WPD_OBJECT_DATE_MODIFIED);
  (*keys)->Add(WPD_OBJECT_CONTENT_TYPE);
  (*keys)->Add(WPD_FUNCTIONAL_OBJECT_CATEGORY);
  return S_OK;
}

HRESULT GetObjectValues(IPortableDeviceContent* content,
                        const std::wstring& object_id,
                        ComPtr<IPortableDeviceValues>* values) {
  ComPtr<IPortableDeviceProperties> properties;
  HRESULT hr = content->Properties(&properties);
  if (FAILED(hr)) {
    return hr;
  }
  ComPtr<IPortableDeviceKeyCollection> keys;
  hr = CreatePropertiesKeyCollection(&keys);
  if (FAILED(hr)) {
    return hr;
  }
  return properties->GetValues(object_id.c_str(), keys.Get(),
                               values->ReleaseAndGetAddressOf());
}

std::wstring GetStringValue(IPortableDeviceValues* values,
                            REFPROPERTYKEY key) {
  if (values == nullptr) {
    return L"";
  }
  LPWSTR raw = nullptr;
  const HRESULT hr = values->GetStringValue(key, &raw);
  if (FAILED(hr) || raw == nullptr) {
    return L"";
  }
  std::wstring output(raw);
  CoTaskMemFree(raw);
  return output;
}

std::optional<int64_t> GetSizeValue(IPortableDeviceValues* values) {
  if (values == nullptr) {
    return std::nullopt;
  }
  ULONGLONG raw = 0;
  const HRESULT hr = values->GetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, &raw);
  if (FAILED(hr)) {
    return std::nullopt;
  }
  return static_cast<int64_t>(raw);
}

std::optional<int64_t> PropVariantToUnixMs(const PROPVARIANT& value) {
  FILETIME file_time{};
  if (value.vt == VT_DATE) {
    SYSTEMTIME system_time{};
    if (!VariantTimeToSystemTime(value.date, &system_time)) {
      return std::nullopt;
    }
    if (!SystemTimeToFileTime(&system_time, &file_time)) {
      return std::nullopt;
    }
  } else if (value.vt == VT_FILETIME) {
    file_time = value.filetime;
  } else {
    return std::nullopt;
  }

  ULARGE_INTEGER ticks{};
  ticks.LowPart = file_time.dwLowDateTime;
  ticks.HighPart = file_time.dwHighDateTime;
  if (ticks.QuadPart < 116444736000000000ULL) {
    return std::nullopt;
  }
  const uint64_t epoch_100ns = ticks.QuadPart - 116444736000000000ULL;
  return static_cast<int64_t>(epoch_100ns / 10000ULL);
}

std::optional<int64_t> GetModifiedAtValue(IPortableDeviceValues* values) {
  if (values == nullptr) {
    return std::nullopt;
  }
  PROPVARIANT value;
  PropVariantInit(&value);
  const HRESULT hr = values->GetValue(WPD_OBJECT_DATE_MODIFIED, &value);
  if (FAILED(hr)) {
    PropVariantClear(&value);
    return std::nullopt;
  }
  const auto output = PropVariantToUnixMs(value);
  PropVariantClear(&value);
  return output;
}

bool IsDirectoryType(IPortableDeviceValues* values) {
  if (values == nullptr) {
    return false;
  }
  GUID content_type = GUID_NULL;
  if (FAILED(values->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &content_type))) {
    return false;
  }
  if (IsEqualGUID(content_type, WPD_CONTENT_TYPE_FOLDER) ||
      IsEqualGUID(content_type, WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT)) {
    return true;
  }
  return false;
}

std::vector<std::wstring> EnumerateObjectIds(IPortableDeviceContent* content,
                                             const std::wstring& parent_id) {
  std::vector<std::wstring> object_ids;
  ComPtr<IEnumPortableDeviceObjectIDs> enumerator;
  HRESULT hr = content->EnumObjects(0, parent_id.c_str(), nullptr,
                                    &enumerator);
  if (FAILED(hr)) {
    return object_ids;
  }

  while (true) {
    PWSTR raw_object_id = nullptr;
    ULONG fetched = 0;
    hr = enumerator->Next(1, &raw_object_id, &fetched);
    if (hr == S_FALSE || fetched == 0) {
      break;
    }
    if (FAILED(hr) || raw_object_id == nullptr) {
      break;
    }
    object_ids.emplace_back(raw_object_id);
    CoTaskMemFree(raw_object_id);
  }
  return object_ids;
}

ResolvedNode BuildNode(IPortableDeviceContent* content,
                       const std::wstring& object_id,
                       const std::string& parent_path) {
  ComPtr<IPortableDeviceValues> values;
  const HRESULT hr = GetObjectValues(content, object_id, &values);

  std::wstring raw_name;
  if (SUCCEEDED(hr)) {
    raw_name = GetStringValue(values.Get(), WPD_STORAGE_DESCRIPTION);
  }
  if (raw_name.empty()) {
    raw_name = GetStringValue(values.Get(), WPD_OBJECT_NAME);
  }
  if (raw_name.empty()) {
    raw_name = GetStringValue(values.Get(), WPD_OBJECT_ORIGINAL_FILE_NAME);
  }

  std::string name = SanitizePathSegment(WideToUtf8(raw_name));
  if (name.empty()) {
    name = WideToUtf8(object_id);
  }

  ResolvedNode node;
  node.object_id_wide = object_id;
  node.name = name;
  node.is_directory = IsDirectoryType(values.Get());
  node.size_bytes = node.is_directory ? std::nullopt : GetSizeValue(values.Get());
  node.modified_at_ms = GetModifiedAtValue(values.Get());
  node.path = parent_path == "/" ? "/" + name : parent_path + "/" + name;
  return node;
}

std::vector<StorageRoot> ListStorageRoots(IPortableDeviceContent* content) {
  std::vector<StorageRoot> roots;
  std::set<std::string> used_labels;
  int fallback_index = 1;

  for (const auto& object_id : EnumerateObjectIds(content, WPD_DEVICE_OBJECT_ID)) {
    const ResolvedNode node = BuildNode(content, object_id, "/");
    if (!node.is_directory) {
      continue;
    }
    std::string label = node.name;
    if (label.empty()) {
      label = "Storage " + std::to_string(fallback_index);
    }
    const std::string base_label = label;
    int suffix = 2;
    while (!used_labels.insert(ToLowerAscii(label)).second) {
      label = base_label + " (" + std::to_string(suffix) + ")";
      suffix++;
    }
    StorageRoot root;
    root.object_id_wide = object_id;
    root.label = label;
    root.path = "/" + label;
    roots.push_back(std::move(root));
    fallback_index++;
  }

  std::sort(roots.begin(), roots.end(),
            [](const StorageRoot& a, const StorageRoot& b) {
              return ToLowerAscii(a.label) < ToLowerAscii(b.label);
            });
  return roots;
}

std::vector<ResolvedNode> ListChildren(IPortableDeviceContent* content,
                                       const std::wstring& parent_id,
                                       const std::string& parent_path) {
  std::vector<ResolvedNode> children;
  for (const auto& object_id : EnumerateObjectIds(content, parent_id)) {
    children.push_back(BuildNode(content, object_id, parent_path));
  }
  std::sort(children.begin(), children.end(),
            [](const ResolvedNode& a, const ResolvedNode& b) {
              if (a.is_directory != b.is_directory) {
                return a.is_directory > b.is_directory;
              }
              return ToLowerAscii(a.name) < ToLowerAscii(b.name);
            });
  return children;
}

ResolvedNode MakeRootNode() {
  ResolvedNode node;
  node.object_id_wide = WPD_DEVICE_OBJECT_ID;
  node.path = "/";
  node.name = "/";
  node.is_directory = true;
  return node;
}

std::optional<ResolvedNode> ResolvePath(const SessionState& session,
                                        const std::string& requested_path) {
  const std::string normalized_path = NormalizePath(requested_path);
  if (normalized_path == "/") {
    return MakeRootNode();
  }

  const std::vector<std::string> segments = SplitPath(normalized_path);
  if (segments.empty()) {
    return std::nullopt;
  }

  const std::string first_segment = segments.front();
  auto root_it = std::find_if(
      session.storage_roots.begin(), session.storage_roots.end(),
      [&](const StorageRoot& root) {
        return root.label == first_segment ||
               ToLowerAscii(root.label) == ToLowerAscii(first_segment);
      });
  if (root_it == session.storage_roots.end()) {
    return std::nullopt;
  }

  ResolvedNode current;
  current.object_id_wide = root_it->object_id_wide;
  current.path = root_it->path;
  current.name = root_it->label;
  current.is_directory = true;

  for (size_t i = 1; i < segments.size(); i++) {
    const auto children =
        ListChildren(session.content.Get(), current.object_id_wide, current.path);
    const std::string segment = segments[i];
    auto child_it = std::find_if(children.begin(), children.end(),
                                 [&](const ResolvedNode& child) {
                                   return child.name == segment ||
                                          ToLowerAscii(child.name) ==
                                              ToLowerAscii(segment);
                                 });
    if (child_it == children.end()) {
      return std::nullopt;
    }
    current = *child_it;
  }

  return current;
}

EncodableMap BuildEntryMap(const ResolvedNode& node) {
  EncodableMap map;
  map[EncodableValue("path")] = EncodableValue(node.path);
  map[EncodableValue("name")] = EncodableValue(node.name);
  map[EncodableValue("isDirectory")] = EncodableValue(node.is_directory);
  map[EncodableValue("sizeBytes")] = node.size_bytes.has_value()
                                         ? EncodableValue(*node.size_bytes)
                                         : EncodableValue();
  map[EncodableValue("modifiedAtMs")] = node.modified_at_ms.has_value()
                                            ? EncodableValue(*node.modified_at_ms)
                                            : EncodableValue();
  return map;
}

std::optional<UsbDeviceRecord> FindDeviceById(const std::string& device_id) {
  for (const auto& device : EnumerateDevices()) {
    if (device.id == device_id) {
      return device;
    }
  }
  return std::nullopt;
}

std::vector<uint8_t> ReadObjectBytes(IPortableDeviceContent* content,
                                     const std::wstring& object_id,
                                     std::string* error_out) {
  std::vector<uint8_t> bytes;
  ComPtr<IPortableDeviceResources> resources;
  HRESULT hr = content->Transfer(&resources);
  if (FAILED(hr)) {
    if (error_out != nullptr) {
      *error_out = HResultMessage(hr, "Unable to access portable device resources.");
    }
    return bytes;
  }

  DWORD optimal_buffer_size = 0;
  ComPtr<IStream> stream;
  hr = resources->GetStream(object_id.c_str(), WPD_RESOURCE_DEFAULT, STGM_READ,
                            &optimal_buffer_size, &stream);
  if (FAILED(hr)) {
    if (error_out != nullptr) {
      *error_out = HResultMessage(hr, "Unable to open object read stream.");
    }
    return bytes;
  }

  const ULONG chunk_size =
      optimal_buffer_size > 0 ? optimal_buffer_size : 64 * 1024;
  std::vector<uint8_t> buffer(chunk_size);
  while (true) {
    ULONG read = 0;
    hr = stream->Read(buffer.data(), chunk_size, &read);
    if (FAILED(hr)) {
      if (error_out != nullptr) {
        *error_out = HResultMessage(hr, "Unable to read bytes from object stream.");
      }
      bytes.clear();
      return bytes;
    }
    if (read == 0) {
      break;
    }
    bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + read);
    if (hr == S_FALSE) {
      break;
    }
  }

  return bytes;
}

}  // namespace

void UsbSyncPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<EncodableValue>>(
          registrar->messenger(), "usb_mtp_client",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<UsbSyncPlugin>();

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

UsbSyncPlugin::UsbSyncPlugin() {
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  com_initialized_ = SUCCEEDED(hr) || hr == S_FALSE;
}

UsbSyncPlugin::~UsbSyncPlugin() {
  g_sessions.clear();
  if (com_initialized_) {
    CoUninitialize();
  }
}

void UsbSyncPlugin::HandleMethodCall(
    const flutter::MethodCall<EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<EncodableValue>> result) {
  if (!com_initialized_) {
    result->Error("com_init_failed",
                  "Unable to initialize COM for Windows portable device access.");
    return;
  }

  const auto* arguments = method_call.arguments();

  if (method_call.method_name() == "listDevices") {
    const DeviceFilter filter = ParseFilter(arguments);
    EncodableList devices;
    for (const auto& device : EnumerateDevices()) {
      if (MatchesFilter(filter, device)) {
        devices.emplace_back(BuildDeviceMap(device));
      }
    }
    result->Success(EncodableValue(devices));
    return;
  }

  const auto* args_map =
      arguments == nullptr ? nullptr : std::get_if<EncodableMap>(arguments);

  if (method_call.method_name() == "openDevice") {
    if (args_map == nullptr) {
      result->Error("invalid_args", "Missing method arguments.");
      return;
    }

    const std::string device_id = ParseString(FindMapValue(*args_map, "deviceId"));
    if (device_id.empty()) {
      result->Error("invalid_args", "Missing required argument: deviceId.");
      return;
    }

    const auto maybe_device = FindDeviceById(device_id);
    if (!maybe_device.has_value()) {
      result->Error("device_not_found", "Device not found: " + device_id);
      return;
    }

    ComPtr<IPortableDevice> device;
    ComPtr<IPortableDeviceContent> content;
    const HRESULT hr =
        OpenPortableDevice(maybe_device->object_id_wide, &device, &content);
    if (FAILED(hr)) {
      result->Error("open_failed",
                    HResultMessage(hr, "Unable to open Windows portable device."));
      return;
    }

    SessionState session;
    session.session_id = NextSessionId(device_id);
    session.device_id = device_id;
    session.device = std::move(device);
    session.content = std::move(content);
    session.storage_roots = ListStorageRoots(session.content.Get());

    if (session.storage_roots.empty()) {
      result->Error("open_failed",
                    "Portable device did not expose any browseable storage roots.");
      return;
    }

    EncodableMap response;
    response[EncodableValue("sessionId")] = EncodableValue(session.session_id);
    response[EncodableValue("deviceId")] = EncodableValue(device_id);
    g_sessions[session.session_id] = std::move(session);
    result->Success(EncodableValue(response));
    return;
  }

  if (method_call.method_name() == "listEntries") {
    if (args_map == nullptr) {
      result->Error("invalid_args", "Missing method arguments.");
      return;
    }

    const std::string session_id =
        ParseString(FindMapValue(*args_map, "sessionId"));
    std::string requested_path = ParseString(FindMapValue(*args_map, "path"));
    if (requested_path.empty()) {
      requested_path = "/";
    }
    if (session_id.empty()) {
      result->Error("invalid_args", "Missing required argument: sessionId.");
      return;
    }

    const auto session_it = g_sessions.find(session_id);
    if (session_it == g_sessions.end()) {
      result->Error("session_not_found",
                    "Session does not exist or is closed.");
      return;
    }

    const auto node = ResolvePath(session_it->second, requested_path);
    if (!node.has_value()) {
      result->Error("invalid_path",
                    "Path does not exist on the portable device: " +
                        NormalizePath(requested_path));
      return;
    }
    if (!node->is_directory) {
      result->Error("invalid_path",
                    "Path is not a directory: " + NormalizePath(requested_path));
      return;
    }

    EncodableList entries;
    if (node->path == "/") {
      for (const auto& root : session_it->second.storage_roots) {
        ResolvedNode root_node;
        root_node.object_id_wide = root.object_id_wide;
        root_node.path = root.path;
        root_node.name = root.label;
        root_node.is_directory = true;
        entries.emplace_back(BuildEntryMap(root_node));
      }
    } else {
      for (const auto& child :
           ListChildren(session_it->second.content.Get(), node->object_id_wide,
                        node->path)) {
        entries.emplace_back(BuildEntryMap(child));
      }
    }

    result->Success(EncodableValue(entries));
    return;
  }

  if (method_call.method_name() == "readBytes") {
    if (args_map == nullptr) {
      result->Error("invalid_args", "Missing method arguments.");
      return;
    }

    const std::string session_id =
        ParseString(FindMapValue(*args_map, "sessionId"));
    const std::string requested_path = ParseString(FindMapValue(*args_map, "path"));
    if (session_id.empty() || requested_path.empty()) {
      result->Error("invalid_args",
                    "Missing required arguments: sessionId and path.");
      return;
    }

    const auto session_it = g_sessions.find(session_id);
    if (session_it == g_sessions.end()) {
      result->Error("session_not_found",
                    "Session does not exist or is closed.");
      return;
    }

    const auto node = ResolvePath(session_it->second, requested_path);
    if (!node.has_value() || node->is_directory) {
      result->Error("invalid_path", "Path is not a file: " + NormalizePath(requested_path));
      return;
    }

    std::string read_error;
    const std::vector<uint8_t> bytes =
        ReadObjectBytes(session_it->second.content.Get(), node->object_id_wide,
                        &read_error);
    if (!read_error.empty()) {
      result->Error("read_failed", read_error);
      return;
    }

    result->Success(EncodableValue(bytes));
    return;
  }

  if (method_call.method_name() == "closeSession") {
    if (args_map == nullptr) {
      result->Error("invalid_args", "Missing method arguments.");
      return;
    }
    const std::string session_id =
        ParseString(FindMapValue(*args_map, "sessionId"));
    if (session_id.empty()) {
      result->Error("invalid_args", "Missing required argument: sessionId.");
      return;
    }
    g_sessions.erase(session_id);
    result->Success();
    return;
  }

  result->NotImplemented();
}

}  // namespace usb_sync
