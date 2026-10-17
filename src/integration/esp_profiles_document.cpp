// OpenTelemetry profiles (v1development) JSON exporter.
//
// opentelemetry-cpp has no profiles SDK, so — unlike the trace/metric exporters
// that drive the vendored SDK — this writes the ProfilesData document directly.
// It uses the same OTLP/JSON wire form as the other signals and lives here so
// all OTLP signal emission stays in one component. Where the document goes is
// the application's choice, through the ProfilesExporter it installs
// (esp_profiles_exporter.hpp) - the stand-in for the exporter interface
// opentelemetry-cpp has for every other signal.

// Only compiled when CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED (see
// CMakeLists.txt) — export_profiles()'s only caller (esp_profiling.cpp) is
// gated the same way.

#include "esp_profiling.hpp"

#include "sdkconfig.h"

extern "C" {
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "json_generator.h"
}

#include "esp_git_ref.hpp"
#include "esp_profiles_exporter.hpp"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/trace/span_id.h"

#include <array>
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
// attribute_table[0], referenced by the mapping.
constexpr int kAttrBuildId = 0;

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

// FreeRTOS task names are arbitrary bytes, and json_generator fails the whole
// document on a string that is not UTF-8; such a name keeps its ASCII bytes.
std::string thread_name(const char* task_name) {
  if (task_name == nullptr) {
    return "?";
  }
  json_gen_str_t probe;
  json_gen_str_start_measure(&probe);
  json_gen_start_array(&probe);
  json_gen_arr_set_string(&probe, task_name);
  json_gen_end_array(&probe);
  std::string name(task_name);
  if (json_gen_str_end(&probe) < 0) {
    for (char& c : name) {
      if (static_cast<unsigned char>(c) >= 0x80) {
        c = '?';
      }
    }
  }
  return name;
}

struct Sample {
  // Into Document::stack_locations; the stack's depth gives the length.
  std::size_t first_location;
  std::array<int, 2> attribute_indices;
  int attribute_count;
};

// Every string, location and attribute the samples refer to, resolved to its
// index before anything is written: the samples come before the dictionary
// they point into.
struct Document {
  const ProfileStack* stacks;
  std::size_t count;
  std::vector<std::string> strings = {"", "samples", "count", "process.executable.build_id"};
  std::vector<uint32_t> locations;
  std::vector<std::pair<int, std::string>> attributes;
  std::vector<int> stack_locations;
  std::vector<Sample> samples;
  std::string time_unix_nano;
  std::string duration_nano;
  std::string profile_id;
};

class Resolver {
 public:
  Resolver(const ProfileStack* stacks, std::size_t count, int64_t time_unix_nano,
           int64_t duration_nano) {
    doc_.stacks = stacks;
    doc_.count = count;
    doc_.time_unix_nano = std::to_string(time_unix_nano);
    doc_.duration_nano = std::to_string(duration_nano);
    doc_.profile_id = random_profile_id();
    const esp_app_desc_t* app = esp_app_get_description();
    doc_.attributes.emplace_back(kStrBuildId,
                                 hex(app->app_elf_sha256, sizeof(app->app_elf_sha256)));
  }

  Document resolve() && {
    doc_.samples.reserve(doc_.count);
    for (std::size_t i = 0; i < doc_.count; ++i) {
      add_sample(doc_.stacks[i]);
    }
    return std::move(doc_);
  }

 private:
  // Pyroscope turns sample attributes into labels and requires valid
  // Prometheus label names (no dots): thread_name, not thread.name. The
  // span_id attribute is what feeds Pyroscope's span-profiles API (the OTLP
  // link_table is ignored by Pyroscope 1.18.1, so no links are emitted).
  void add_sample(const ProfileStack& stack) {
    Sample sample = {doc_.stack_locations.size(), {}, 0};
    for (int d = 0; d < stack.depth; ++d) {
      doc_.stack_locations.push_back(location_for(stack.addresses[d]));
    }
    sample.attribute_indices[sample.attribute_count++] =
        attribute_for("thread_name", thread_name(stack.task_name));
    if (stack.has_span) {
      sample.attribute_indices[sample.attribute_count++] =
          attribute_for("span_id", span_id_hex(stack.span_id));
    }
    doc_.samples.push_back(sample);
  }

  int intern(const std::string& s) {
    for (size_t i = 0; i < doc_.strings.size(); ++i) {
      if (doc_.strings[i] == s) return static_cast<int>(i);
    }
    doc_.strings.push_back(s);
    return static_cast<int>(doc_.strings.size() - 1);
  }

  int location_for(uint32_t addr) {
    auto it = location_index_.find(addr);
    if (it != location_index_.end()) return it->second;
    int idx = static_cast<int>(doc_.locations.size());
    location_index_.emplace(addr, idx);
    doc_.locations.push_back(addr);
    return idx;
  }

  int attribute_for(const char* key, const std::string& value) {
    std::string dedupe_key = std::string(key) + "\x1f" + value;
    auto it = attribute_index_.find(dedupe_key);
    if (it != attribute_index_.end()) return it->second;
    int idx = static_cast<int>(doc_.attributes.size());
    doc_.attributes.emplace_back(intern(key), value);
    attribute_index_.emplace(std::move(dedupe_key), idx);
    return idx;
  }

  Document doc_;
  std::unordered_map<uint32_t, int> location_index_;
  std::unordered_map<std::string, int> attribute_index_;
};

void write_string_value(json_gen_str_t* j, const char* name, const char* s) {
  json_gen_push_object(j, name);
  json_gen_obj_set_string(j, "stringValue", s);
  json_gen_pop_object(j);
}

void write_resource_attribute(json_gen_str_t* j, const char* key, const char* value) {
  json_gen_start_object(j);
  json_gen_obj_set_string(j, "key", key);
  write_string_value(j, "value", value);
  json_gen_end_object(j);
}

// Dotless keys, matching Grafana's convention; empty value omits the attribute.
void write_optional_resource_attribute(json_gen_str_t* j, const char* key, const char* value) {
  if (value != nullptr && value[0] != '\0') {
    write_resource_attribute(j, key, value);
  }
}

void write_resource(json_gen_str_t* j) {
  json_gen_push_object(j, "resource");
  json_gen_push_array(j, "attributes");
  write_resource_attribute(j, "service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME);
  write_optional_resource_attribute(j, "service_repository",
                                    CONFIG_ESP_OPENTELEMETRY_SERVICE_REPOSITORY);
  write_optional_resource_attribute(j, "service_git_ref", esp_opentelemetry::current_git_ref());
  write_optional_resource_attribute(j, "service_root_path",
                                    CONFIG_ESP_OPENTELEMETRY_SERVICE_ROOT_PATH);
  json_gen_pop_array(j);
  json_gen_pop_object(j);
}

void write_sample_type(json_gen_str_t* j, const char* name) {
  json_gen_push_object(j, name);
  json_gen_obj_set_int(j, "typeStrindex", kStrSamples);
  json_gen_obj_set_int(j, "unitStrindex", kStrCount);
  json_gen_pop_object(j);
}

void write_profile(json_gen_str_t* j, const Document& doc) {
  json_gen_start_object(j);
  write_sample_type(j, "sampleType");
  json_gen_push_array(j, "samples");
  for (std::size_t i = 0; i < doc.count; ++i) {
    json_gen_start_object(j);
    json_gen_obj_set_int(j, "stackIndex", static_cast<int>(i));
    json_gen_push_array(j, "values");
    json_gen_arr_set_string(j, std::to_string(doc.stacks[i].count).c_str());
    json_gen_pop_array(j);
    json_gen_push_array(j, "attributeIndices");
    const Sample& sample = doc.samples[i];
    for (int a = 0; a < sample.attribute_count; ++a) {
      json_gen_arr_set_int(j, sample.attribute_indices[a]);
    }
    json_gen_pop_array(j);
    json_gen_end_object(j);
  }
  json_gen_pop_array(j);
  json_gen_obj_set_string(j, "timeUnixNano", doc.time_unix_nano.c_str());
  json_gen_obj_set_string(j, "durationNano", doc.duration_nano.c_str());
  write_sample_type(j, "periodType");
  // Pyroscope requires a non-zero period to derive the profile metric name;
  // one count == one sample.
  json_gen_obj_set_string(j, "period", "1");
  json_gen_obj_set_string(j, "profileId", doc.profile_id.c_str());
  json_gen_end_object(j);
}

void write_dictionary(json_gen_str_t* j, const Document& doc) {
  json_gen_push_object(j, "dictionary");

  json_gen_push_array(j, "stringTable");
  for (const auto& str : doc.strings) {
    json_gen_arr_set_string(j, str.c_str());
  }
  json_gen_pop_array(j);

  json_gen_push_array(j, "functionTable");
  json_gen_pop_array(j);

  json_gen_push_array(j, "mappingTable");
  json_gen_start_object(j);
  json_gen_obj_set_string(j, "memoryStart", "0");
  json_gen_obj_set_string(j, "memoryLimit", "0");
  json_gen_obj_set_string(j, "fileOffset", "0");
  json_gen_obj_set_int(j, "filenameStrindex", kStrEmpty);
  json_gen_push_array(j, "attributeIndices");
  json_gen_arr_set_int(j, kAttrBuildId);
  json_gen_pop_array(j);
  json_gen_end_object(j);
  json_gen_pop_array(j);

  json_gen_push_array(j, "locationTable");
  for (uint32_t addr : doc.locations) {
    json_gen_start_object(j);
    json_gen_obj_set_int(j, "mappingIndex", 0);
    json_gen_obj_set_string(j, "address", std::to_string(addr).c_str());
    json_gen_end_object(j);
  }
  json_gen_pop_array(j);

  json_gen_push_array(j, "stackTable");
  for (std::size_t i = 0; i < doc.count; ++i) {
    json_gen_start_object(j);
    json_gen_push_array(j, "locationIndices");
    const std::size_t first = doc.samples[i].first_location;
    for (int d = 0; d < doc.stacks[i].depth; ++d) {
      json_gen_arr_set_int(j, doc.stack_locations[first + d]);
    }
    json_gen_pop_array(j);
    json_gen_end_object(j);
  }
  json_gen_pop_array(j);

  json_gen_push_array(j, "attributeTable");
  for (const auto& [key_strindex, value] : doc.attributes) {
    json_gen_start_object(j);
    json_gen_obj_set_int(j, "keyStrindex", key_strindex);
    write_string_value(j, "value", value.c_str());
    json_gen_end_object(j);
  }
  json_gen_pop_array(j);

  json_gen_pop_object(j);
}

void write_document(json_gen_str_t* j, const Document& doc) {
  json_gen_start_object(j);
  json_gen_push_array(j, "resourceProfiles");
  json_gen_start_object(j);
  write_resource(j);
  json_gen_push_array(j, "scopeProfiles");
  json_gen_start_object(j);
  json_gen_push_object(j, "scope");
  json_gen_obj_set_string(j, "name", "esp-profiling");
  json_gen_obj_set_string(j, "version", "1.0.0");
  json_gen_pop_object(j);
  json_gen_push_array(j, "profiles");
  write_profile(j, doc);
  json_gen_pop_array(j);
  json_gen_end_object(j);
  json_gen_pop_array(j);
  json_gen_end_object(j);
  json_gen_pop_array(j);
  write_dictionary(j, doc);
  json_gen_end_object(j);
}

}  // namespace

void export_profiles(const ProfileStack* stacks, std::size_t count,
                     int64_t time_unix_nano, int64_t duration_nano) {
  if (g_exporter == nullptr || count == 0) {
    return;
  }
  const Document doc = Resolver(stacks, count, time_unix_nano, duration_nano).resolve();

  json_gen_str_t jstr;
  json_gen_str_start_measure(&jstr);
  write_document(&jstr, doc);
  const int size = json_gen_str_end(&jstr);
  if (size < 0) {
    ESP_LOGW(TAG, "failed to measure profiles JSON: %d", size);
    return;
  }
  // The document grows with the profile; the tasks around the export need
  // internal RAM for FreeRTOS objects, sockets and DMA.
  char* body = static_cast<char*>(heap_caps_malloc_prefer(
      static_cast<size_t>(size), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT));
  if (body == nullptr) {
    ESP_LOGW(TAG, "no memory for a %d-byte profiles JSON", size);
    return;
  }
  json_gen_str_start(&jstr, body, size, nullptr, nullptr);
  write_document(&jstr, doc);
  if (json_gen_str_end(&jstr) < 0) {
    ESP_LOGW(TAG, "failed to serialize profiles JSON");
  } else {
    (void)g_exporter->Export(body, static_cast<std::size_t>(size - 1));
  }
  heap_caps_free(body);
}

}  // namespace esp_opentelemetry
