// Profiles exporter interface.
//
// opentelemetry-cpp has no profiles SDK, so there is no PushMetricExporter
// equivalent to implement. This is the seam that stands in for one, shaped like
// the SDK's other exporter interfaces so profiles are selected the same way as
// every other signal:
//
//   esp_opentelemetry_profiling_setup(
//       std::make_unique<esp_opentelemetry::JtagProfilesExporter>());
//
// Implementations: JtagProfilesExporter (esp_jtag_exporters.hpp),
// MakeOtlpHttpProfilesExporter() (esp_otlp_http_exporters.hpp), and
// MakeConsoleProfilesExporter() below.
//
// The document is OTLP/JSON with unsymbolized program counters; the host end
// is normally the symbolizer (tools/symbolizer/), which resolves them against
// the firmware ELF before forwarding to a collector.

#pragma once

#include "sdkconfig.h"

#include <cstddef>
#include <memory>

namespace esp_opentelemetry {

class ProfilesExporter {
 public:
  virtual ~ProfilesExporter() = default;

  // Send one complete ProfilesData JSON document. Returns false when it could
  // not be delivered; the caller logs and moves on, since a profile is a
  // sample and the next window replaces it.
  virtual bool Export(const char* body, std::size_t size) noexcept = 0;
};

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OSTREAM)
// Prints each document between PROFILE_JSON_BEGIN / PROFILE_JSON_END markers.
// Profiles have no human-readable renderer, so this is the raw JSON. The other
// signals get their console exporters from the SDK; this one has no SDK to get
// it from.
std::unique_ptr<ProfilesExporter> MakeConsoleProfilesExporter();
#endif

}  // namespace esp_opentelemetry
