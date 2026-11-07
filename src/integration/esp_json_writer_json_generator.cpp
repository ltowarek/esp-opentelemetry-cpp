// Validated before json_generator sees it, which trusts its caller to balance containers.

#include "esp_otlp_json_backends.hpp"

extern "C" {
#include "esp_log.h"
#include "json_generator.h"
}

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "opentelemetry/exporters/otlp/otlp_json_writer.h"
#include "opentelemetry/exporters/otlp/otlp_json_writer_factory.h"
#include "opentelemetry/nostd/string_view.h"

namespace esp_opentelemetry {

namespace {

namespace otlp_api = opentelemetry::exporter::otlp;

constexpr const char* TAG = "esp_otel_json_writer";

// RFC 4648 base64 with padding.
std::string base64_encode(const std::uint8_t* data, std::size_t size) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((size + 2) / 3) * 4);
  std::size_t i = 0;
  for (; i + 3 <= size; i += 3) {
    const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                            (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                            static_cast<std::uint32_t>(data[i + 2]);
    out.push_back(kAlphabet[(n >> 18) & 0x3f]);
    out.push_back(kAlphabet[(n >> 12) & 0x3f]);
    out.push_back(kAlphabet[(n >> 6) & 0x3f]);
    out.push_back(kAlphabet[n & 0x3f]);
  }
  const std::size_t remaining = size - i;
  if (remaining == 1) {
    const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
    out.push_back(kAlphabet[(n >> 18) & 0x3f]);
    out.push_back(kAlphabet[(n >> 12) & 0x3f]);
    out.push_back('=');
    out.push_back('=');
  } else if (remaining == 2) {
    const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                            (static_cast<std::uint32_t>(data[i + 1]) << 8);
    out.push_back(kAlphabet[(n >> 18) & 0x3f]);
    out.push_back(kAlphabet[(n >> 12) & 0x3f]);
    out.push_back(kAlphabet[(n >> 6) & 0x3f]);
    out.push_back('=');
  }
  return out;
}

constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";  // U+FFFD

// A well-formed sequence's length and the range its second byte must fall in;
// the bytes after it are always 0x80..0xBF (Unicode Table 3-7). A length of 0
// marks a byte that cannot start a sequence.
struct Utf8Lead {
  std::size_t length;
  unsigned char second_min;
  unsigned char second_max;
};

Utf8Lead utf8_lead(unsigned char byte) {
  if (byte < 0x80) return {1, 0, 0};
  if (byte >= 0xC2 && byte <= 0xDF) return {2, 0x80, 0xBF};
  if (byte == 0xE0) return {3, 0xA0, 0xBF};
  if (byte == 0xED) return {3, 0x80, 0x9F};
  if (byte >= 0xE1 && byte <= 0xEF) return {3, 0x80, 0xBF};
  if (byte == 0xF0) return {4, 0x90, 0xBF};
  if (byte >= 0xF1 && byte <= 0xF3) return {4, 0x80, 0xBF};
  if (byte == 0xF4) return {4, 0x80, 0x8F};
  return {0, 0, 0};
}

// How many bytes of the sequence that `lead` starts are well-formed, out of
// the `available` bytes from its lead byte on.
std::size_t matched_length(const unsigned char* bytes, std::size_t available, Utf8Lead lead) {
  if (lead.length == 0) {
    return 0;
  }
  for (std::size_t matched = 1; matched < lead.length && matched < available; ++matched) {
    const unsigned char min = matched == 1 ? lead.second_min : 0x80;
    const unsigned char max = matched == 1 ? lead.second_max : 0xBF;
    if (bytes[matched] < min || bytes[matched] > max) {
      return matched;
    }
  }
  return lead.length < available ? lead.length : available;
}

// json_generator fails the whole document on a string that is not UTF-8. Each
// ill-formed sequence becomes one U+FFFD, as Unicode recommends; a character
// left incomplete at the end is dropped instead. Returns `text` itself when it
// is already well-formed, otherwise the repair, held in `repaired`.
std::string_view well_formed_utf8(std::string_view text, std::string& repaired) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  const std::size_t size = text.size();
  bool repairing = false;
  for (std::size_t i = 0; i < size;) {
    const Utf8Lead lead = utf8_lead(bytes[i]);
    const std::size_t matched = matched_length(bytes + i, size - i, lead);
    const bool whole = lead.length > 0 && matched == lead.length;
    if (!whole && !repairing) {
      repaired.assign(text.substr(0, i));
      repairing = true;
    }
    if (whole) {
      if (repairing) {
        repaired.append(text.substr(i, matched));
      }
    } else if (lead.length > 0 && i + matched == size) {
      break;
    } else {
      repaired += kReplacementCharacter;
    }
    i += matched > 0 ? matched : 1;
  }
  return repairing ? std::string_view(repaired) : text;
}

class JsonGeneratorJsonWriter final : public otlp_api::JsonWriter {
 public:
  JsonGeneratorJsonWriter() noexcept {
    json_gen_str_start(&gen_, chunk_, sizeof(chunk_), &Flush, this);
  }

  void BeginObject() noexcept override { Open(Container::kObject); }
  void EndObject() noexcept override { Close(Container::kObject); }
  void BeginArray() noexcept override { Open(Container::kArray); }
  void EndArray() noexcept override { Close(Container::kArray); }

  void Key(opentelemetry::nostd::string_view key) noexcept override {
    if (!ok_) {
      return;
    }
    if (ended_) {
      Fail("Key() called after ToString()");
      return;
    }
    if (stack_.empty() || stack_.back() != Container::kObject) {
      Fail("Key() called outside an object");
      return;
    }
    if (has_key_) {
      Fail("Key() called twice without an intervening value");
      return;
    }
    // json_generator takes names as C strings.
    key_.assign(well_formed_utf8(std::string_view(key.data(), key.size()), utf8_));
    has_key_ = true;
  }

  void WriteNull() noexcept override {
    Write([this](const char* name) { return json_gen_obj_set_null(&gen_, name); },
          [this] { return json_gen_arr_set_null(&gen_); });
  }

  void WriteString(opentelemetry::nostd::string_view text) noexcept override {
    const std::string_view value =
        well_formed_utf8(std::string_view(text.data(), text.size()), utf8_);
    Write(
        [this, value](const char* name) {
          return json_gen_obj_set_string_len(&gen_, name, value.data(), value.size());
        },
        [this, value] { return json_gen_arr_set_string_len(&gen_, value.data(), value.size()); });
  }
  void WriteInt32(std::int32_t value) noexcept override { WriteInt64(value); }
  void WriteUInt32(std::uint32_t value) noexcept override { WriteInt64(value); }

  void WriteInt64(std::int64_t value) noexcept override {
    Write([this, value](const char* name) { return json_gen_obj_set_int64(&gen_, name, value); },
          [this, value] { return json_gen_arr_set_int64(&gen_, value); });
  }

  // json_generator has no unsigned 64-bit value. Above INT64_MAX the digits go
  // in as a raw member, which it only offers inside an object.
  void WriteUInt64(std::uint64_t value) noexcept override {
    if (value <= static_cast<std::uint64_t>(INT64_MAX)) {
      WriteInt64(static_cast<std::int64_t>(value));
      return;
    }
    Write(
        [this, value](const char* name) {
          return json_gen_push_object_str(&gen_, name, std::to_string(value).c_str());
        },
        [this] {
          Fail("json_generator cannot write a uint64 above INT64_MAX outside an object");
          return JSON_GEN_OK;
        });
  }

  void WriteDouble(double value) noexcept override {
    Write([this, value](const char* name) { return json_gen_obj_set_double(&gen_, name, value); },
          [this, value] { return json_gen_arr_set_double(&gen_, value); });
  }

  void WriteBool(bool value) noexcept override {
    Write([this, value](const char* name) { return json_gen_obj_set_bool(&gen_, name, value); },
          [this, value] { return json_gen_arr_set_bool(&gen_, value); });
  }
  void WriteBytes(const std::uint8_t* data, std::size_t size) noexcept override {
    WriteString(base64_encode(data, size));
  }

  bool ok() const noexcept override { return ok_; }

  std::string ToString() noexcept override {
    if (!ended_) {
      ended_ = true;
      Check(json_gen_str_end(&gen_));
    }
    return std::move(out_);
  }

 private:
  enum class Container { kObject, kArray };

  // Where the value about to be written goes.
  enum class Slot { kRejected, kNamed, kUnnamed };

  static void Flush(char* chunk, void* self) {
    static_cast<JsonGeneratorJsonWriter*>(self)->out_.append(chunk);
  }

  Slot Claim() noexcept {
    if (!ok_) {
      return Slot::kRejected;
    }
    if (ended_) {
      Fail("a value was written after ToString()");
      return Slot::kRejected;
    }
    if (stack_.empty()) {
      if (root_set_) {
        Fail("a value was written after the top-level value was already complete");
        return Slot::kRejected;
      }
      root_set_ = true;
      return Slot::kUnnamed;
    }
    if (stack_.back() == Container::kArray) {
      return Slot::kUnnamed;
    }
    if (!has_key_) {
      Fail("a value was written without a preceding Key()");
      return Slot::kRejected;
    }
    has_key_ = false;
    return Slot::kNamed;
  }

  // Writes a value with the json_gen_obj_set_* call when it follows a Key(),
  // the json_gen_arr_set_* one otherwise; the top level writes like an element.
  template <typename Named, typename Unnamed>
  void Write(Named named, Unnamed unnamed) noexcept {
    const Slot slot = Claim();
    if (slot == Slot::kNamed) {
      Check(named(key_.c_str()));
    } else if (slot == Slot::kUnnamed) {
      Check(unnamed());
    }
  }

  void Open(Container kind) noexcept {
    const Slot slot = Claim();
    if (slot == Slot::kRejected) {
      return;
    }
    const bool object = kind == Container::kObject;
    if (slot == Slot::kNamed) {
      Check(object ? json_gen_push_object(&gen_, key_.c_str())
                   : json_gen_push_array(&gen_, key_.c_str()));
    } else {
      Check(object ? json_gen_start_object(&gen_) : json_gen_start_array(&gen_));
    }
    stack_.push_back(kind);
  }

  void Close(Container kind) noexcept {
    if (!ok_) {
      return;
    }
    if (ended_) {
      Fail("a container was closed after ToString()");
      return;
    }
    if (stack_.empty() || stack_.back() != kind) {
      Fail("mismatched Begin/End call");
      return;
    }
    if (kind == Container::kObject && has_key_) {
      Fail("object closed with a Key() that has no value");
      return;
    }
    stack_.pop_back();
    Check(kind == Container::kObject ? json_gen_end_object(&gen_) : json_gen_end_array(&gen_));
  }

  // json_generator returns a negative JSON_GEN_ERR_* on failure; json_gen_str_end
  // returns the document length on success.
  void Check(int result) noexcept {
    if (result < 0 && ok_) {
      ok_ = false;
      ESP_LOGE(TAG, "json_generator failed: %d", result);
    }
  }

  void Fail(const char* reason) noexcept {
    if (!ok_) {
      return;
    }
    ok_ = false;
    ESP_LOGE(TAG, "%s", reason);
  }

  json_gen_str_t gen_;
  char chunk_[256];
  std::string out_;
  std::vector<Container> stack_;
  std::string key_;
  std::string utf8_;
  bool has_key_ = false;
  bool root_set_ = false;
  bool ended_ = false;
  bool ok_ = true;
};

class JsonGeneratorJsonWriterFactoryImpl final : public otlp_api::JsonWriterFactory {
 public:
  std::unique_ptr<otlp_api::JsonWriter> Create() override {
    return std::make_unique<JsonGeneratorJsonWriter>();
  }
};

}  // namespace

std::shared_ptr<otlp_api::JsonWriterFactory> JsonGeneratorJsonWriterFactory() {
  static const std::shared_ptr<otlp_api::JsonWriterFactory> instance =
      std::make_shared<JsonGeneratorJsonWriterFactoryImpl>();
  return instance;
}

}  // namespace esp_opentelemetry
