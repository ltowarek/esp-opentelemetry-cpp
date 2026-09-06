// A generic JSON emitter for ESP-IDF, expressed as a token stream (begin/end
// object, begin/end array, key, and typed value overloads) rather than a
// document. This mirrors an interface proposed to opentelemetry-cpp
// (exporters/otlp/otlp_json_writer.h there, nlohmann-backed) so that a
// second, independent backend — cJSON, already a dependency of this
// component — can validate the interface's shape before it goes upstream
// for review. Abstracting a document would require abstracting node
// identity, ownership and allocators across libraries whose shapes
// disagree; a token stream shares no data structure at all.
//
// The interface carries no OTLP awareness: hex-encoded IDs, integer enums
// and 64-bit-as-string are mapping-layer decisions made by the caller, who
// picks the token (e.g. WriteString with an already hex-encoded value)
// accordingly. WriteNull is the one addition beyond typed scalars and
// bytes, needed because JSON itself has no way to represent "value
// present, fields absent" other than null.
//
// Every method is noexcept and returns void. A failed write does not throw
// or abort; it sets a sticky flag queryable through ok() and, on the first
// failure only, logs through ESP_LOGE. This is required for ESP-IDF
// builds, which compile with exceptions and RTTI disabled.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace esp_opentelemetry {

class JsonWriter {
 public:
  JsonWriter() = default;
  JsonWriter(const JsonWriter&) = delete;
  JsonWriter(JsonWriter&&) = delete;
  JsonWriter& operator=(const JsonWriter&) = delete;
  JsonWriter& operator=(JsonWriter&&) = delete;
  virtual ~JsonWriter() = default;

  virtual void BeginObject() noexcept = 0;
  virtual void EndObject() noexcept = 0;
  virtual void BeginArray() noexcept = 0;
  virtual void EndArray() noexcept = 0;

  // Sets the key for the next value written inside the currently open
  // object. Only valid directly inside BeginObject()/EndObject().
  virtual void Key(std::string_view key) noexcept = 0;

  virtual void WriteNull() noexcept = 0;
  virtual void WriteString(std::string_view value) noexcept = 0;
  virtual void WriteInt32(std::int32_t value) noexcept = 0;
  virtual void WriteInt64(std::int64_t value) noexcept = 0;
  virtual void WriteUInt32(std::uint32_t value) noexcept = 0;
  virtual void WriteUInt64(std::uint64_t value) noexcept = 0;
  virtual void WriteDouble(double value) noexcept = 0;
  virtual void WriteBool(bool value) noexcept = 0;
  virtual void WriteBytes(const std::uint8_t* data, std::size_t size) noexcept = 0;

  // False once any write has failed (malformed token stream). Sticky:
  // once false, stays false for the lifetime of this writer.
  virtual bool ok() const noexcept = 0;

  // Returns the document accumulated so far, serialized as a string. Valid
  // to call regardless of ok(); a failed writer returns whatever was
  // accumulated before the first failure.
  virtual std::string ToString() noexcept = 0;
};

// The only backend this component provides: cJSON (espressif/cjson), already
// a dependency of the profiles exporter this interface was extracted from.
std::unique_ptr<JsonWriter> MakeCjsonJsonWriter();

}  // namespace esp_opentelemetry
