// The SDK-side JsonWriter/JsonReader backends this component supplies.
//
// opentelemetry-cpp reaches JSON through two abstract seams —
// exporter::otlp::JsonWriter for request bodies and exporter::otlp::JsonReader
// for export responses — and ships one backend for each, built on
// nlohmann-json. That backend is excluded here
// (OTELCPP_WITH_JSON_WRITER_NLOHMANN=OFF): nlohmann is a heavyweight
// header-only library whose error path is exceptions, which ESP-IDF builds
// disable. cJSON, which this component already depends on, provides both
// instead.
//
// With the bundled backend excluded, the SDK has no default to fall back on:
// its GetDefaultJsonWriterFactory() terminates. Every exporter this component
// constructs must therefore be given these factories explicitly — the OTLP/JSON
// exporters, the protobuf-backed OTLP/HTTP exporters (which serialise JSON
// bodies through the same seam), and the OTLP file exporters behind the JTAG
// transport alike.

#pragma once

#include "opentelemetry/exporters/otlp/otlp_json_reader_factory.h"
#include "opentelemetry/exporters/otlp/otlp_json_writer_factory.h"

#include <memory>

namespace esp_opentelemetry {

// Both are process-wide singletons: a factory holds no per-exporter state, and
// the SDK takes them by shared_ptr expecting to share one.
std::shared_ptr<opentelemetry::exporter::otlp::JsonWriterFactory>
CjsonJsonWriterFactory();

std::shared_ptr<opentelemetry::exporter::otlp::JsonReaderFactory>
CjsonJsonReaderFactory();

}  // namespace esp_opentelemetry
