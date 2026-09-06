#include "esp_otlp_json_backends.hpp"

#include "esp_json_writer.hpp"

#include <cJSON.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "opentelemetry/exporters/otlp/otlp_json_reader.h"
#include "opentelemetry/exporters/otlp/otlp_json_writer.h"
#include "opentelemetry/nostd/string_view.h"

namespace otlp_api = opentelemetry::exporter::otlp;

namespace esp_opentelemetry {

namespace {

// Forwards the SDK's token vocabulary onto this component's own JsonWriter,
// which cJSON already backs for the profiles exporter. The two interfaces are
// token-for-token identical and deliberately stay separate types: the profiles
// exporter emits through JsonWriter in builds where no signal is enabled and
// the SDK headers are not on the include path at all.
class CjsonOtlpJsonWriter final : public otlp_api::JsonWriter {
 public:
  CjsonOtlpJsonWriter() : impl_(MakeCjsonJsonWriter()) {}

  void BeginObject() noexcept override { impl_->BeginObject(); }
  void EndObject() noexcept override { impl_->EndObject(); }
  void BeginArray() noexcept override { impl_->BeginArray(); }
  void EndArray() noexcept override { impl_->EndArray(); }

  void Key(opentelemetry::nostd::string_view key) noexcept override {
    impl_->Key(std::string_view(key.data(), key.size()));
  }

  void WriteNull() noexcept override { impl_->WriteNull(); }

  void WriteString(opentelemetry::nostd::string_view value) noexcept override {
    impl_->WriteString(std::string_view(value.data(), value.size()));
  }

  void WriteInt32(std::int32_t value) noexcept override { impl_->WriteInt32(value); }
  void WriteInt64(std::int64_t value) noexcept override { impl_->WriteInt64(value); }
  void WriteUInt32(std::uint32_t value) noexcept override { impl_->WriteUInt32(value); }
  void WriteUInt64(std::uint64_t value) noexcept override { impl_->WriteUInt64(value); }
  void WriteDouble(double value) noexcept override { impl_->WriteDouble(value); }
  void WriteBool(bool value) noexcept override { impl_->WriteBool(value); }

  void WriteBytes(const std::uint8_t* data, std::size_t size) noexcept override {
    impl_->WriteBytes(data, size);
  }

  bool ok() const noexcept override { return impl_->ok(); }
  std::string ToString() noexcept override { return impl_->ToString(); }

 private:
  // Qualified: JsonWriter unqualified names the SDK's interface here.
  const std::unique_ptr<esp_opentelemetry::JsonWriter> impl_;
};

class CjsonJsonWriterFactoryImpl final : public otlp_api::JsonWriterFactory {
 public:
  std::unique_ptr<otlp_api::JsonWriter> Create() override {
    return std::unique_ptr<otlp_api::JsonWriter>(new CjsonOtlpJsonWriter());
  }
};

// Parses a decimal integer the way OTLP/JSON writes a 64-bit field, which is
// as a string. The digits are validated before std::strtoll sees them, because
// strtoll would otherwise silently accept leading whitespace, a "0x" prefix and
// trailing characters; strtoll is then left to do the one thing that check
// cannot, which is reject a value outside the 64-bit range.
bool ParseDecimalInt64(const char* text, std::int64_t& value) noexcept {
  const std::string_view digits(text);
  if (digits.empty()) {
    return false;
  }
  const std::size_t first_digit = (digits[0] == '-' || digits[0] == '+') ? 1 : 0;
  if (first_digit >= digits.size()) {
    return false;
  }
  for (std::size_t i = first_digit; i < digits.size(); ++i) {
    if (digits[i] < '0' || digits[i] > '9') {
      return false;
    }
  }

  errno = 0;
  const long long parsed = std::strtoll(text, nullptr, 10);
  if (errno == ERANGE) {
    return false;
  }
  value = static_cast<std::int64_t>(parsed);
  return true;
}

// A JsonReader over a cJSON document. cJSON reports every failure as a null
// return or a false type predicate and never longjmps or aborts, so each
// method stays noexcept without any guarding of its own.
class CjsonJsonReader final : public otlp_api::JsonReader {
 public:
  ~CjsonJsonReader() override { Reset(); }

  bool Parse(opentelemetry::nostd::string_view document) noexcept override {
    Reset();
    root_ = cJSON_ParseWithLength(document.data(), document.size());
    if (root_ == nullptr) {
      return false;
    }
    if (!cJSON_IsObject(root_)) {
      Reset();
      return false;
    }
    stack_.push_back(root_);
    return true;
  }

  bool EnterObject(opentelemetry::nostd::string_view key) noexcept override {
    const cJSON* member = Find(key);
    if (member == nullptr || !cJSON_IsObject(member)) {
      return false;
    }
    stack_.push_back(member);
    return true;
  }

  void LeaveObject() noexcept override {
    if (stack_.size() > 1) {
      stack_.pop_back();
    }
  }

  bool GetInt64(opentelemetry::nostd::string_view key,
                std::int64_t& value) noexcept override {
    const cJSON* member = Find(key);
    if (member == nullptr) {
      return false;
    }
    if (cJSON_IsString(member)) {
      return ParseDecimalInt64(member->valuestring, value);
    }
    if (!cJSON_IsNumber(member)) {
      return false;
    }
    // cJSON holds every number as a double, so a JSON-number field only
    // survives this read up to 2^53. OTLP/JSON writes 64-bit fields as
    // strings, which is the branch above and is exact; a producer that emits
    // them as numbers is outside what this reader can represent, and a value
    // it cannot represent exactly is refused rather than silently rounded.
    const double number = member->valuedouble;
    if (!std::isfinite(number) || number != std::floor(number)) {
      return false;
    }
    constexpr double kInt64Min = -9223372036854775808.0;
    // 2^63 exactly; int64 max itself is not representable as a double, so the
    // bound is exclusive.
    constexpr double kInt64Limit = 9223372036854775808.0;
    if (number < kInt64Min || number >= kInt64Limit) {
      return false;
    }
    value = static_cast<std::int64_t>(number);
    return true;
  }

  bool GetString(opentelemetry::nostd::string_view key,
                 std::string& value) noexcept override {
    const cJSON* member = Find(key);
    if (member == nullptr || !cJSON_IsString(member)) {
      return false;
    }
    value = member->valuestring;
    return true;
  }

 private:
  const cJSON* Find(opentelemetry::nostd::string_view key) noexcept {
    if (stack_.empty()) {
      return nullptr;
    }
    // cJSON has no length-bounded lookup, and a JsonReader key is a
    // string_view over a caller's buffer that need not be terminated.
    const std::string name(key.data(), key.size());
    return cJSON_GetObjectItemCaseSensitive(stack_.back(), name.c_str());
  }

  void Reset() noexcept {
    stack_.clear();
    if (root_ != nullptr) {
      cJSON_Delete(root_);
      root_ = nullptr;
    }
  }

  cJSON* root_ = nullptr;
  // Ancestors of the current object, innermost last. Empty until Parse()
  // succeeds; the root is never popped.
  std::vector<const cJSON*> stack_;
};

class CjsonJsonReaderFactoryImpl final : public otlp_api::JsonReaderFactory {
 public:
  std::unique_ptr<otlp_api::JsonReader> Create() override {
    return std::unique_ptr<otlp_api::JsonReader>(new CjsonJsonReader());
  }
};

}  // namespace

std::shared_ptr<otlp_api::JsonWriterFactory> CjsonJsonWriterFactory() {
  static const std::shared_ptr<otlp_api::JsonWriterFactory> instance =
      std::make_shared<CjsonJsonWriterFactoryImpl>();
  return instance;
}

std::shared_ptr<otlp_api::JsonReaderFactory> CjsonJsonReaderFactory() {
  static const std::shared_ptr<otlp_api::JsonReaderFactory> instance =
      std::make_shared<CjsonJsonReaderFactoryImpl>();
  return instance;
}

}  // namespace esp_opentelemetry
