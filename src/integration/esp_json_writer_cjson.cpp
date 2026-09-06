// cJSON-backed JsonWriter. Accumulates tokens into a cJSON document tree —
// unlike the nlohmann backend this interface was proven against upstream,
// cJSON containers already preserve insertion order, so no reordering
// happens between the token stream and the serialized output.
//
// Every write first validates the token stream itself (no value without a
// preceding key inside an object, no key outside an object, balanced
// Begin/End) before touching cJSON, so cJSON never sees a call it could
// reject other than an allocation failure. Combined with cJSON's C error
// model (NULL on failure, no exceptions), this is what lets every method
// stay noexcept.

#include "esp_json_writer.hpp"

extern "C" {
#include "esp_log.h"
}

#include <cJSON.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace esp_opentelemetry {

namespace {

constexpr const char* TAG = "esp_otel_json_writer";

// RFC 4648 base64 with padding, matching the "AAEC/w==" shape the interface
// contract requires of WriteBytes. cJSON has no encoder of its own to reuse
// here: unlike WriteString/WriteInt32/etc., where cJSON's own string
// escaping and number formatting can be trusted directly, bytes-as-base64
// is a mapping decision the writer itself must make.
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

class CjsonJsonWriter final : public JsonWriter {
 public:
  ~CjsonJsonWriter() override {
    if (root_ != nullptr) {
      cJSON_Delete(root_);
    }
  }

  void BeginObject() noexcept override { OpenContainer(cJSON_CreateObject, Container::kObject); }
  void EndObject() noexcept override { CloseContainer(Container::kObject); }
  void BeginArray() noexcept override { OpenContainer(cJSON_CreateArray, Container::kArray); }
  void EndArray() noexcept override { CloseContainer(Container::kArray); }

  void Key(std::string_view key) noexcept override {
    if (!ok_) {
      return;
    }
    if (stack_.empty() || stack_.back().kind != Container::kObject) {
      Fail("Key() called outside an object");
      return;
    }
    if (has_pending_key_) {
      Fail("Key() called twice without an intervening value");
      return;
    }
    pending_key_ = std::string(key);
    has_pending_key_ = true;
  }

  void WriteNull() noexcept override { WriteValue(cJSON_CreateNull); }

  void WriteString(std::string_view value) noexcept override {
    const std::string owned(value);
    WriteValue([&owned] { return cJSON_CreateString(owned.c_str()); });
  }

  void WriteInt32(std::int32_t value) noexcept override {
    WriteValue([value] { return cJSON_CreateNumber(static_cast<double>(value)); });
  }

  void WriteUInt32(std::uint32_t value) noexcept override {
    WriteValue([value] { return cJSON_CreateNumber(static_cast<double>(value)); });
  }

  void WriteInt64(std::int64_t value) noexcept override {
    // cJSON numbers are doubles, which cannot represent the full int64_t
    // range exactly (a gap in this vocabulary vs. the nlohmann backend,
    // whose json::number_integer_t is a native int64_t): a raw node prints
    // the exact decimal text instead of round-tripping through a double.
    const std::string text = std::to_string(value);
    WriteValue([&text] { return cJSON_CreateRaw(text.c_str()); });
  }

  void WriteUInt64(std::uint64_t value) noexcept override {
    const std::string text = std::to_string(value);
    WriteValue([&text] { return cJSON_CreateRaw(text.c_str()); });
  }

  void WriteDouble(double value) noexcept override {
    WriteValue([value] { return cJSON_CreateNumber(value); });
  }

  void WriteBool(bool value) noexcept override {
    WriteValue([value] { return cJSON_CreateBool(value); });
  }

  void WriteBytes(const std::uint8_t* data, std::size_t size) noexcept override {
    const std::string encoded = base64_encode(data, size);
    WriteValue([&encoded] { return cJSON_CreateString(encoded.c_str()); });
  }

  bool ok() const noexcept override { return ok_; }

  std::string ToString() noexcept override {
    if (root_ == nullptr) {
      return std::string();
    }
    char* printed = cJSON_PrintUnformatted(root_);
    if (printed == nullptr) {
      return std::string();
    }
    std::string result(printed);
    cJSON_free(printed);
    return result;
  }

 private:
  enum class Container { kObject, kArray };

  struct Frame {
    cJSON* node;
    Container kind;
  };

  // Creates a value with `make` (any of cJSON's cJSON_Create* factories) and
  // attaches it wherever the current token-stream state says it belongs
  // (root, next array element, or the pending object key). `make` runs only
  // once the attach point is validated, so a malformed call never touches
  // cJSON.
  template <typename Make>
  void WriteValue(Make make) noexcept {
    if (!ClaimSlot()) {
      return;
    }
    cJSON* item = make();
    if (item == nullptr) {
      Fail("failed to allocate a JSON value");
      return;
    }
    Attach(item);
  }

  template <typename Make>
  void OpenContainer(Make make, Container kind) noexcept {
    if (!ClaimSlot()) {
      return;
    }
    cJSON* item = make();
    if (item == nullptr) {
      Fail("failed to allocate a JSON container");
      return;
    }
    // Attach() may free `item` on failure (e.g. cJSON_AddItemToObject running
    // out of memory duplicating the key), so the stack must never see it
    // unless the attach actually succeeded.
    if (Attach(item)) {
      stack_.push_back(Frame{item, kind});
    }
  }

  // Validates that a value may be written next (root not already set /
  // pending key present inside an object) and, on success, consumes that
  // state (marks the root as set, or clears has_pending_key_). Returns
  // false, having already called Fail(), if the token stream is malformed.
  bool ClaimSlot() noexcept {
    if (!ok_) {
      return false;
    }
    if (stack_.empty()) {
      if (root_set_) {
        Fail("a value was written after the top-level value was already complete");
        return false;
      }
      root_set_ = true;
      return true;
    }
    const Frame& frame = stack_.back();
    if (frame.kind == Container::kArray) {
      return true;
    }
    if (!has_pending_key_) {
      Fail("a value was written without a preceding Key()");
      return false;
    }
    has_pending_key_ = false;
    return true;
  }

  // Attaches an already-created item at the point ClaimSlot() just
  // validated. Must be called immediately after a successful ClaimSlot(),
  // with no intervening call that could change pending_key_ or the stack.
  // Returns whether the item is now reachable from the document: false means
  // it has already been deleted and the caller must not keep the pointer.
  bool Attach(cJSON* item) noexcept {
    if (stack_.empty()) {
      root_ = item;
      return true;
    }
    Frame& frame = stack_.back();
    const cJSON_bool added = (frame.kind == Container::kArray)
                                  ? cJSON_AddItemToArray(frame.node, item)
                                  : cJSON_AddItemToObject(frame.node, pending_key_.c_str(), item);
    if (!added) {
      cJSON_Delete(item);
      Fail("failed to attach a JSON value to its container");
      return false;
    }
    return true;
  }

  void CloseContainer(Container kind) noexcept {
    if (!ok_) {
      return;
    }
    if (stack_.empty() || stack_.back().kind != kind) {
      Fail("mismatched Begin/End call");
      return;
    }
    if (kind == Container::kObject && has_pending_key_) {
      Fail("object closed with a Key() that has no value");
      return;
    }
    stack_.pop_back();
  }

  void Fail(const char* reason) noexcept {
    if (!ok_) {
      return;
    }
    ok_ = false;
    ESP_LOGE(TAG, "%s", reason);
  }

  cJSON* root_ = nullptr;
  std::vector<Frame> stack_;
  std::string pending_key_;
  bool has_pending_key_ = false;
  bool root_set_ = false;
  bool ok_ = true;
};

}  // namespace

std::unique_ptr<JsonWriter> MakeCjsonJsonWriter() {
  return std::make_unique<CjsonJsonWriter>();
}

}  // namespace esp_opentelemetry
