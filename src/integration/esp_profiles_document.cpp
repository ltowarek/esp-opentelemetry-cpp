// OpenTelemetry profiles (v1development) JSON exporter.
//
// opentelemetry-cpp has no profiles SDK, so — unlike the trace/metric exporters
// that drive the vendored SDK — this builds the ProfilesData document directly
// with cJSON. It uses the same OTLP/JSON wire form as the other signals and
// lives here so all OTLP signal emission stays in one component. Where the
// document goes is the application's choice, through the ProfilesExporter it
// installs (esp_profiles_exporter.hpp) - the stand-in for the exporter
// interface opentelemetry-cpp has for every other signal.

// Only compiled when CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED (see
// CMakeLists.txt) — export_profiles()'s only caller (esp_profiling.cpp) is
// gated the same way.

#include "esp_profiling.hpp"

#include "sdkconfig.h"

extern "C" {
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
}

#include <cJSON.h>

#include "esp_git_ref.hpp"
#include "esp_profiles_exporter.hpp"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/trace/span_id.h"

#include <cstring>
#include <memory>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

namespace esp_opentelemetry {

namespace {
// Set once during setup, read by the export task; no lock needed because
// setup runs before the sampler starts.
std::unique_ptr<ProfilesExporter> g_exporter;
}  // namespace

void set_profiles_exporter(std::unique_ptr<ProfilesExporter> exporter) {
  g_exporter = std::move(exporter);
}

namespace {

constexpr const char* TAG = "esp_otel_profiles";

// Fixed string_table prefix; index 0 must be "".
enum : int { kStrEmpty = 0, kStrSamples = 1, kStrCount = 2, kStrBuildId = 3 };

std::string hex(const uint8_t* bytes, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s(n * 2, '0');
  for (size_t i = 0; i < n; ++i) {
    s[2 * i] = d[bytes[i] >> 4];
    s[2 * i + 1] = d[bytes[i] & 0xf];
  }
  return s;
}

std::string random_profile_id() {
  uint8_t id[16];
  esp_fill_random(id, sizeof(id));
  return hex(id, sizeof(id));
}

// span_id is SpanId-shaped (8 bytes), so its hex form goes through the SDK's
// own encoder rather than the generic hex() above (which stays for build_id
// and profile_id, neither of which is a SpanId/TraceId).
std::string span_id_hex(const uint8_t (&span_id)[8]) {
  char buf[2 * opentelemetry::trace::SpanId::kSize];
  opentelemetry::trace::SpanId(opentelemetry::nostd::span<const uint8_t, 8>(span_id, 8))
      .ToLowerBase16(opentelemetry::nostd::span<char, 2 * opentelemetry::trace::SpanId::kSize>(
          buf, sizeof(buf)));
  return std::string(buf, sizeof(buf));
}

cJSON* value_string(const char* s) {
  cJSON* v = cJSON_CreateObject();
  cJSON_AddStringToObject(v, "stringValue", s);
  return v;
}

cJSON* attribute(int key_strindex, const char* string_value) {
  cJSON* a = cJSON_CreateObject();
  cJSON_AddNumberToObject(a, "keyStrindex", key_strindex);
  cJSON_AddItemToObject(a, "value", value_string(string_value));
  return a;
}

}  // namespace

void export_profiles(const ProfileStack* stacks, std::size_t count,
                     int64_t time_unix_nano, int64_t duration_nano) {
  if (g_exporter == nullptr || count == 0) {
    return;
  }

  const esp_app_desc_t* app = esp_app_get_description();
  const std::string build_id = hex(app->app_elf_sha256, sizeof(app->app_elf_sha256));

  std::vector<std::string> strings = {"", "samples", "count",
                                      "process.executable.build_id"};
  auto intern = [&](const std::string& s) -> int {
    for (size_t i = 0; i < strings.size(); ++i) {
      if (strings[i] == s) return static_cast<int>(i);
    }
    strings.push_back(s);
    return static_cast<int>(strings.size() - 1);
  };

  cJSON* location_table = cJSON_CreateArray();
  std::unordered_map<uint32_t, int> addr_index;
  auto location_for = [&](uint32_t addr) -> int {
    auto it = addr_index.find(addr);
    if (it != addr_index.end()) return it->second;
    int idx = static_cast<int>(addr_index.size());
    addr_index.emplace(addr, idx);
    cJSON* loc = cJSON_CreateObject();
    cJSON_AddNumberToObject(loc, "mappingIndex", 0);
    cJSON_AddStringToObject(loc, "address", std::to_string(addr).c_str());
    cJSON_AddItemToArray(location_table, loc);
    return idx;
  };

  // attribute_table[0] = build_id (referenced by the mapping); thread_name and
  // span_id entries follow, deduplicated and referenced per sample.
  cJSON* attribute_table = cJSON_CreateArray();
  cJSON_AddItemToArray(attribute_table, attribute(kStrBuildId, build_id.c_str()));
  std::unordered_map<std::string, int> attr_index;
  // cJSON arrays are linked lists with no cached length, so track the next
  // index ourselves instead of calling cJSON_GetArraySize() (O(n) traversal)
  // on every insert.
  int next_attr_idx = 1;
  // Pyroscope turns sample attributes into labels and requires valid
  // Prometheus label names (no dots): thread_name, not thread.name. The
  // span_id attribute is what feeds Pyroscope's span-profiles API (the OTLP
  // link_table is ignored by Pyroscope 1.18.1, so no links are emitted).
  auto attr_for = [&](const char* key, const std::string& value) -> int {
    std::string dedupe_key = std::string(key) + "\x1f" + value;
    auto it = attr_index.find(dedupe_key);
    if (it != attr_index.end()) return it->second;
    int idx = next_attr_idx++;
    cJSON_AddItemToArray(attribute_table, attribute(intern(key), value.c_str()));
    attr_index.emplace(std::move(dedupe_key), idx);
    return idx;
  };
  auto thread_attr_for = [&](const char* task) -> int {
    return attr_for("thread_name", task ? task : "?");
  };

  cJSON* stack_table = cJSON_CreateArray();
  cJSON* samples = cJSON_CreateArray();
  // Reused across iterations (clear() keeps its buffer) instead of
  // heap-allocating a fresh vector per sample.
  std::vector<int> loc_indices;
  for (std::size_t i = 0; i < count; ++i) {
    const ProfileStack& s = stacks[i];

    loc_indices.clear();
    loc_indices.reserve(s.depth);
    for (int d = 0; d < s.depth; ++d) {
      loc_indices.push_back(location_for(s.addresses[d]));
    }
    cJSON* stk = cJSON_CreateObject();
    cJSON_AddItemToObject(stk, "locationIndices",
                          cJSON_CreateIntArray(loc_indices.data(),
                                               static_cast<int>(loc_indices.size())));
    cJSON_AddItemToArray(stack_table, stk);

    cJSON* sample = cJSON_CreateObject();
    cJSON_AddNumberToObject(sample, "stackIndex", static_cast<double>(i));
    cJSON* values = cJSON_CreateArray();
    cJSON_AddItemToArray(values, cJSON_CreateString(std::to_string(s.count).c_str()));
    cJSON_AddItemToObject(sample, "values", values);
    int attrs[2] = {thread_attr_for(s.task_name), 0};
    int n_attrs = 1;
    if (s.has_span) {
      attrs[n_attrs++] = attr_for("span_id", span_id_hex(s.span_id));
    }
    cJSON_AddItemToObject(sample, "attributeIndices", cJSON_CreateIntArray(attrs, n_attrs));
    cJSON_AddItemToArray(samples, sample);
  }

  cJSON* mapping = cJSON_CreateObject();
  cJSON_AddStringToObject(mapping, "memoryStart", "0");
  cJSON_AddStringToObject(mapping, "memoryLimit", "0");
  cJSON_AddStringToObject(mapping, "fileOffset", "0");
  cJSON_AddNumberToObject(mapping, "filenameStrindex", kStrEmpty);
  int build_id_attr[] = {0};
  cJSON_AddItemToObject(mapping, "attributeIndices", cJSON_CreateIntArray(build_id_attr, 1));
  cJSON* mapping_table = cJSON_CreateArray();
  cJSON_AddItemToArray(mapping_table, mapping);

  cJSON* string_table = cJSON_CreateArray();
  for (const auto& s : strings) {
    cJSON_AddItemToArray(string_table, cJSON_CreateString(s.c_str()));
  }

  cJSON* dictionary = cJSON_CreateObject();
  cJSON_AddItemToObject(dictionary, "stringTable", string_table);
  cJSON_AddItemToObject(dictionary, "functionTable", cJSON_CreateArray());
  cJSON_AddItemToObject(dictionary, "mappingTable", mapping_table);
  cJSON_AddItemToObject(dictionary, "locationTable", location_table);
  cJSON_AddItemToObject(dictionary, "stackTable", stack_table);
  cJSON_AddItemToObject(dictionary, "attributeTable", attribute_table);

  cJSON* sample_type = cJSON_CreateObject();
  cJSON_AddNumberToObject(sample_type, "typeStrindex", kStrSamples);
  cJSON_AddNumberToObject(sample_type, "unitStrindex", kStrCount);

  cJSON* profile = cJSON_CreateObject();
  cJSON_AddItemToObject(profile, "sampleType", sample_type);
  cJSON_AddItemToObject(profile, "samples", samples);
  cJSON_AddStringToObject(profile, "timeUnixNano", std::to_string(time_unix_nano).c_str());
  cJSON_AddStringToObject(profile, "durationNano", std::to_string(duration_nano).c_str());
  cJSON* period_type = cJSON_CreateObject();
  cJSON_AddNumberToObject(period_type, "typeStrindex", kStrSamples);
  cJSON_AddNumberToObject(period_type, "unitStrindex", kStrCount);
  cJSON_AddItemToObject(profile, "periodType", period_type);
  // Pyroscope requires a non-zero period to derive the profile metric name;
  // one count == one sample.
  cJSON_AddStringToObject(profile, "period", "1");
  cJSON_AddStringToObject(profile, "profileId", random_profile_id().c_str());

  cJSON* profiles = cJSON_CreateArray();
  cJSON_AddItemToArray(profiles, profile);

  cJSON* scope = cJSON_CreateObject();
  cJSON_AddStringToObject(scope, "name", "esp-profiling");
  cJSON_AddStringToObject(scope, "version", "1.0.0");
  cJSON* scope_profiles = cJSON_CreateObject();
  cJSON_AddItemToObject(scope_profiles, "scope", scope);
  cJSON_AddItemToObject(scope_profiles, "profiles", profiles);

  cJSON* resource_attr = cJSON_CreateArray();
  cJSON* svc = cJSON_CreateObject();
  cJSON_AddStringToObject(svc, "key", "service.name");
  cJSON_AddItemToObject(svc, "value", value_string(CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME));
  cJSON_AddItemToArray(resource_attr, svc);

  // Dotless keys, matching Grafana's convention; empty value omits the attribute.
  auto add_resource_attr = [&](const char* key, const char* value) {
    if (value == nullptr || value[0] == '\0') {
      return;
    }
    cJSON* attr = cJSON_CreateObject();
    cJSON_AddStringToObject(attr, "key", key);
    cJSON_AddItemToObject(attr, "value", value_string(value));
    cJSON_AddItemToArray(resource_attr, attr);
  };
  add_resource_attr("service_repository", CONFIG_ESP_OPENTELEMETRY_SERVICE_REPOSITORY);
  add_resource_attr("service_git_ref", esp_opentelemetry::current_git_ref());
  add_resource_attr("service_root_path", CONFIG_ESP_OPENTELEMETRY_SERVICE_ROOT_PATH);

  cJSON* resource = cJSON_CreateObject();
  cJSON_AddItemToObject(resource, "attributes", resource_attr);

  cJSON* resource_profiles = cJSON_CreateObject();
  cJSON_AddItemToObject(resource_profiles, "resource", resource);
  cJSON* scope_profiles_arr = cJSON_CreateArray();
  cJSON_AddItemToArray(scope_profiles_arr, scope_profiles);
  cJSON_AddItemToObject(resource_profiles, "scopeProfiles", scope_profiles_arr);

  cJSON* root = cJSON_CreateObject();
  cJSON* resource_profiles_arr = cJSON_CreateArray();
  cJSON_AddItemToArray(resource_profiles_arr, resource_profiles);
  cJSON_AddItemToObject(root, "resourceProfiles", resource_profiles_arr);
  cJSON_AddItemToObject(root, "dictionary", dictionary);

  char* body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (body == nullptr) {
    ESP_LOGW(TAG, "failed to serialize profiles JSON");
    return;
  }

  (void)g_exporter->Export(body, strlen(body));
  cJSON_free(body);
}

}  // namespace esp_opentelemetry
