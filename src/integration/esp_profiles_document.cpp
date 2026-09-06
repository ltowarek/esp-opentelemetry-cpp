// OpenTelemetry profiles (v1development) JSON exporter.
//
// opentelemetry-cpp has no profiles SDK, so — unlike the trace/metric exporters
// that drive the vendored SDK — this builds the ProfilesData document directly
// through esp_json_writer.hpp (cJSON-backed). It uses the same OTLP/JSON wire
// form as the other signals and lives here so all OTLP signal emission stays
// in one component. Where the document goes is the application's choice,
// through the ProfilesExporter it installs (esp_profiles_exporter.hpp) - the
// stand-in for the exporter interface opentelemetry-cpp has for every other
// signal.
//
// Building the document is a two-pass affair: a JsonWriter is a token stream,
// not a document, so it cannot have two containers open in different places
// the way a cJSON subtree can. The dictionary tables (string/location/
// attribute) and the per-sample stack/attribute indices are all discovered
// while walking `stacks`, but `dictionary` is emitted after `resourceProfiles`
// in the wire form. Pass 1 (BuildTables) walks the samples once and captures
// everything into plain vectors; pass 2 (EmitDocument) replays those vectors
// through the writer in final field order.

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
#include "esp_json_writer.hpp"
#include "esp_profiles_exporter.hpp"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/trace/span_id.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
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

// One attributeTable entry: an interned key plus its (always string) value.
struct AttributeEntry {
  int key_strindex;
  std::string value;
};

// One sample plus the stackTable entry it shares an index with (stackIndex
// in the sample refers to this entry's position in the samples/stack vector).
struct SampleEntry {
  std::vector<int> loc_indices;  // -> stackTable[i].locationIndices
  uint32_t count;
  int attr_indices[2];
  int n_attrs;
};

// Everything BuildTables discovers while walking `stacks`, replayed by
// EmitDocument in the document's final field order.
struct ProfilesTables {
  std::vector<std::string> strings = {"", "samples", "count", "process.executable.build_id"};
  std::vector<uint32_t> location_addresses;  // locationTable, first-seen order
  std::vector<AttributeEntry> attributes;    // attributeTable; [0] == build_id
  std::vector<SampleEntry> samples;          // parallel to stackTable
};

ProfilesTables BuildTables(const ProfileStack* stacks, std::size_t count,
                          const std::string& build_id) {
  ProfilesTables t;

  auto intern = [&](const std::string& s) -> int {
    for (size_t i = 0; i < t.strings.size(); ++i) {
      if (t.strings[i] == s) return static_cast<int>(i);
    }
    t.strings.push_back(s);
    return static_cast<int>(t.strings.size() - 1);
  };

  std::unordered_map<uint32_t, int> addr_index;
  auto location_for = [&](uint32_t addr) -> int {
    auto it = addr_index.find(addr);
    if (it != addr_index.end()) return it->second;
    int idx = static_cast<int>(addr_index.size());
    addr_index.emplace(addr, idx);
    t.location_addresses.push_back(addr);
    return idx;
  };

  // attribute_table[0] = build_id (referenced by the mapping); thread_name and
  // span_id entries follow, deduplicated and referenced per sample.
  t.attributes.push_back(AttributeEntry{kStrBuildId, build_id});
  std::unordered_map<std::string, int> attr_index;
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
    t.attributes.push_back(AttributeEntry{intern(key), value});
    attr_index.emplace(std::move(dedupe_key), idx);
    return idx;
  };
  auto thread_attr_for = [&](const char* task) -> int {
    return attr_for("thread_name", task ? task : "?");
  };

  t.samples.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const ProfileStack& s = stacks[i];

    SampleEntry entry;
    entry.loc_indices.reserve(s.depth);
    for (int d = 0; d < s.depth; ++d) {
      entry.loc_indices.push_back(location_for(s.addresses[d]));
    }
    entry.count = s.count;
    entry.attr_indices[0] = thread_attr_for(s.task_name);
    entry.n_attrs = 1;
    if (s.has_span) {
      entry.attr_indices[entry.n_attrs++] = attr_for("span_id", span_id_hex(s.span_id));
    }
    t.samples.push_back(std::move(entry));
  }
  return t;
}

void WriteStringValue(JsonWriter& w, const std::string& s) {
  w.BeginObject();
  w.Key("stringValue");
  w.WriteString(s);
  w.EndObject();
}

// Dotless keys, matching Grafana's convention; an empty value omits the
// attribute entirely.
void WriteResourceAttrIfSet(JsonWriter& w, const char* key, const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return;
  }
  w.BeginObject();
  w.Key("key");
  w.WriteString(key);
  w.Key("value");
  WriteStringValue(w, value);
  w.EndObject();
}

void EmitDocument(JsonWriter& w, const ProfilesTables& t, int64_t time_unix_nano,
                  int64_t duration_nano) {
  w.BeginObject();  // root

  w.Key("resourceProfiles");
  w.BeginArray();
  w.BeginObject();  // resource_profiles

  w.Key("resource");
  w.BeginObject();
  w.Key("attributes");
  w.BeginArray();
  w.BeginObject();
  w.Key("key");
  w.WriteString("service.name");
  w.Key("value");
  WriteStringValue(w, CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME);
  w.EndObject();
  WriteResourceAttrIfSet(w, "service_repository", CONFIG_ESP_OPENTELEMETRY_SERVICE_REPOSITORY);
  WriteResourceAttrIfSet(w, "service_git_ref", esp_opentelemetry::current_git_ref());
  WriteResourceAttrIfSet(w, "service_root_path", CONFIG_ESP_OPENTELEMETRY_SERVICE_ROOT_PATH);
  w.EndArray();   // attributes
  w.EndObject();  // resource

  w.Key("scopeProfiles");
  w.BeginArray();
  w.BeginObject();  // scope_profiles

  w.Key("scope");
  w.BeginObject();
  w.Key("name");
  w.WriteString("esp-profiling");
  w.Key("version");
  w.WriteString("1.0.0");
  w.EndObject();

  w.Key("profiles");
  w.BeginArray();
  w.BeginObject();  // profile

  w.Key("sampleType");
  w.BeginObject();
  w.Key("typeStrindex");
  w.WriteInt32(kStrSamples);
  w.Key("unitStrindex");
  w.WriteInt32(kStrCount);
  w.EndObject();

  w.Key("samples");
  w.BeginArray();
  for (std::size_t i = 0; i < t.samples.size(); ++i) {
    const SampleEntry& s = t.samples[i];
    w.BeginObject();
    w.Key("stackIndex");
    w.WriteInt32(static_cast<int32_t>(i));
    w.Key("values");
    w.BeginArray();
    w.WriteString(std::to_string(s.count));
    w.EndArray();
    w.Key("attributeIndices");
    w.BeginArray();
    for (int a = 0; a < s.n_attrs; ++a) {
      w.WriteInt32(s.attr_indices[a]);
    }
    w.EndArray();
    w.EndObject();
  }
  w.EndArray();  // samples

  w.Key("timeUnixNano");
  w.WriteString(std::to_string(time_unix_nano));
  w.Key("durationNano");
  w.WriteString(std::to_string(duration_nano));

  w.Key("periodType");
  w.BeginObject();
  w.Key("typeStrindex");
  w.WriteInt32(kStrSamples);
  w.Key("unitStrindex");
  w.WriteInt32(kStrCount);
  w.EndObject();

  // Pyroscope requires a non-zero period to derive the profile metric name;
  // one count == one sample.
  w.Key("period");
  w.WriteString("1");
  w.Key("profileId");
  w.WriteString(random_profile_id());

  w.EndObject();  // profile
  w.EndArray();   // profiles

  w.EndObject();  // scope_profiles
  w.EndArray();   // scopeProfiles

  w.EndObject();  // resource_profiles
  w.EndArray();   // resourceProfiles

  w.Key("dictionary");
  w.BeginObject();

  w.Key("stringTable");
  w.BeginArray();
  for (const auto& s : t.strings) {
    w.WriteString(s);
  }
  w.EndArray();

  w.Key("functionTable");
  w.BeginArray();
  w.EndArray();

  w.Key("mappingTable");
  w.BeginArray();
  w.BeginObject();
  w.Key("memoryStart");
  w.WriteString("0");
  w.Key("memoryLimit");
  w.WriteString("0");
  w.Key("fileOffset");
  w.WriteString("0");
  w.Key("filenameStrindex");
  w.WriteInt32(kStrEmpty);
  w.Key("attributeIndices");
  w.BeginArray();
  w.WriteInt32(0);  // references attribute_table[0], the build_id
  w.EndArray();
  w.EndObject();
  w.EndArray();  // mappingTable

  w.Key("locationTable");
  w.BeginArray();
  for (uint32_t addr : t.location_addresses) {
    w.BeginObject();
    w.Key("mappingIndex");
    w.WriteInt32(0);
    w.Key("address");
    w.WriteString(std::to_string(addr));
    w.EndObject();
  }
  w.EndArray();

  w.Key("stackTable");
  w.BeginArray();
  for (const auto& s : t.samples) {
    w.BeginObject();
    w.Key("locationIndices");
    w.BeginArray();
    for (int idx : s.loc_indices) {
      w.WriteInt32(idx);
    }
    w.EndArray();
    w.EndObject();
  }
  w.EndArray();

  w.Key("attributeTable");
  w.BeginArray();
  for (const auto& a : t.attributes) {
    w.BeginObject();
    w.Key("keyStrindex");
    w.WriteInt32(a.key_strindex);
    w.Key("value");
    WriteStringValue(w, a.value);
    w.EndObject();
  }
  w.EndArray();

  w.EndObject();  // dictionary

  w.EndObject();  // root
}

}  // namespace

void export_profiles(const ProfileStack* stacks, std::size_t count,
                     int64_t time_unix_nano, int64_t duration_nano) {
  if (g_exporter == nullptr || count == 0) {
    return;
  }

  const esp_app_desc_t* app = esp_app_get_description();
  const std::string build_id = hex(app->app_elf_sha256, sizeof(app->app_elf_sha256));

  const ProfilesTables tables = BuildTables(stacks, count, build_id);

  auto writer = MakeCjsonJsonWriter();
  EmitDocument(*writer, tables, time_unix_nano, duration_nano);
  if (!writer->ok()) {
    ESP_LOGW(TAG, "failed to build profiles JSON");
    return;
  }

  const std::string body = writer->ToString();
  (void)g_exporter->Export(body.c_str(), body.size());
}

}  // namespace esp_opentelemetry
