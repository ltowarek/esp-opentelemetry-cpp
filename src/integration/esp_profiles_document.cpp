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
#include "esp_log.h"
#include "esp_random.h"
}

#include "esp_git_ref.hpp"
#include "esp_otlp_json_backends.hpp"
#include "esp_profiles_exporter.hpp"
#include "opentelemetry/exporters/otlp/otlp_json_writer.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/nostd/string_view.h"
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

using opentelemetry::exporter::otlp::JsonWriter;

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

// JsonWriter repairs a task name that is not UTF-8.
std::string thread_name(const char* task_name) { return task_name == nullptr ? "?" : task_name; }

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

void write_string_value(JsonWriter& j, const char* name, opentelemetry::nostd::string_view s) {
  j.Key(name);
  j.BeginObject();
  j.Key("stringValue");
  j.WriteString(s);
  j.EndObject();
}

void write_resource_attribute(JsonWriter& j, const char* key, const char* value) {
  j.BeginObject();
  j.Key("key");
  j.WriteString(key);
  write_string_value(j, "value", value);
  j.EndObject();
}

// Dotless keys, matching Grafana's convention; empty value omits the attribute.
void write_optional_resource_attribute(JsonWriter& j, const char* key, const char* value) {
  if (value != nullptr && value[0] != '\0') {
    write_resource_attribute(j, key, value);
  }
}

void write_resource(JsonWriter& j) {
  j.Key("resource");
  j.BeginObject();
  j.Key("attributes");
  j.BeginArray();
  write_resource_attribute(j, "service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME);
  write_optional_resource_attribute(j, "service_repository",
                                    CONFIG_ESP_OPENTELEMETRY_SERVICE_REPOSITORY);
  write_optional_resource_attribute(j, "service_git_ref", esp_opentelemetry::current_git_ref());
  write_optional_resource_attribute(j, "service_root_path",
                                    CONFIG_ESP_OPENTELEMETRY_SERVICE_ROOT_PATH);
  j.EndArray();
  j.EndObject();
}

void write_sample_type(JsonWriter& j, const char* name) {
  j.Key(name);
  j.BeginObject();
  j.Key("typeStrindex");
  j.WriteInt32(kStrSamples);
  j.Key("unitStrindex");
  j.WriteInt32(kStrCount);
  j.EndObject();
}

void write_profile(JsonWriter& j, const Document& doc) {
  j.BeginObject();
  write_sample_type(j, "sampleType");
  j.Key("samples");
  j.BeginArray();
  for (std::size_t i = 0; i < doc.count; ++i) {
    j.BeginObject();
    j.Key("stackIndex");
    j.WriteInt32(static_cast<int>(i));
    j.Key("values");
    j.BeginArray();
    j.WriteString(std::to_string(doc.stacks[i].count));
    j.EndArray();
    j.Key("attributeIndices");
    j.BeginArray();
    const Sample& sample = doc.samples[i];
    for (int a = 0; a < sample.attribute_count; ++a) {
      j.WriteInt32(sample.attribute_indices[a]);
    }
    j.EndArray();
    j.EndObject();
  }
  j.EndArray();
  j.Key("timeUnixNano");
  j.WriteString(doc.time_unix_nano);
  j.Key("durationNano");
  j.WriteString(doc.duration_nano);
  write_sample_type(j, "periodType");
  // Pyroscope requires a non-zero period to derive the profile metric name;
  // one count == one sample.
  j.Key("period");
  j.WriteString("1");
  j.Key("profileId");
  j.WriteString(doc.profile_id);
  j.EndObject();
}

void write_dictionary(JsonWriter& j, const Document& doc) {
  j.Key("dictionary");
  j.BeginObject();

  j.Key("stringTable");
  j.BeginArray();
  for (const auto& str : doc.strings) {
    j.WriteString(str);
  }
  j.EndArray();

  j.Key("functionTable");
  j.BeginArray();
  j.EndArray();

  j.Key("mappingTable");
  j.BeginArray();
  j.BeginObject();
  j.Key("memoryStart");
  j.WriteString("0");
  j.Key("memoryLimit");
  j.WriteString("0");
  j.Key("fileOffset");
  j.WriteString("0");
  j.Key("filenameStrindex");
  j.WriteInt32(kStrEmpty);
  j.Key("attributeIndices");
  j.BeginArray();
  j.WriteInt32(kAttrBuildId);
  j.EndArray();
  j.EndObject();
  j.EndArray();

  j.Key("locationTable");
  j.BeginArray();
  for (uint32_t addr : doc.locations) {
    j.BeginObject();
    j.Key("mappingIndex");
    j.WriteInt32(0);
    j.Key("address");
    j.WriteString(std::to_string(addr));
    j.EndObject();
  }
  j.EndArray();

  j.Key("stackTable");
  j.BeginArray();
  for (std::size_t i = 0; i < doc.count; ++i) {
    j.BeginObject();
    j.Key("locationIndices");
    j.BeginArray();
    const std::size_t first = doc.samples[i].first_location;
    for (int d = 0; d < doc.stacks[i].depth; ++d) {
      j.WriteInt32(doc.stack_locations[first + d]);
    }
    j.EndArray();
    j.EndObject();
  }
  j.EndArray();

  j.Key("attributeTable");
  j.BeginArray();
  for (const auto& [key_strindex, value] : doc.attributes) {
    j.BeginObject();
    j.Key("keyStrindex");
    j.WriteInt32(key_strindex);
    write_string_value(j, "value", value);
    j.EndObject();
  }
  j.EndArray();

  j.EndObject();
}

void write_document(JsonWriter& j, const Document& doc) {
  j.BeginObject();
  j.Key("resourceProfiles");
  j.BeginArray();
  j.BeginObject();
  write_resource(j);
  j.Key("scopeProfiles");
  j.BeginArray();
  j.BeginObject();
  j.Key("scope");
  j.BeginObject();
  j.Key("name");
  j.WriteString("esp-profiling");
  j.Key("version");
  j.WriteString("1.0.0");
  j.EndObject();
  j.Key("profiles");
  j.BeginArray();
  write_profile(j, doc);
  j.EndArray();
  j.EndObject();
  j.EndArray();
  j.EndObject();
  j.EndArray();
  write_dictionary(j, doc);
  j.EndObject();
}

}  // namespace

void export_profiles(const ProfileStack* stacks, std::size_t count,
                     int64_t time_unix_nano, int64_t duration_nano) {
  if (g_exporter == nullptr || count == 0) {
    return;
  }
  const Document doc = Resolver(stacks, count, time_unix_nano, duration_nano).resolve();

  const std::unique_ptr<JsonWriter> writer = JsonGeneratorJsonWriterFactory()->Create();
  write_document(*writer, doc);
  const std::string body = writer->ToString();
  if (!writer->ok()) {
    ESP_LOGW(TAG, "failed to serialize profiles JSON");
    return;
  }
  (void)g_exporter->Export(body.data(), body.size());
}

}  // namespace esp_opentelemetry
