// Workaround: int32_t is `long` not `int` on Xtensa GCC 13.2; bool/int/pid_t
// do not match any EncodeVarint overload — ambiguous call. Upstream: Abseil.
//
// Workaround for Abseil EncodeVarint ambiguous overloads on Xtensa
// GCC 13.2. On this 32-bit platform, int/pid_t/bool do not match any
// of the four explicit EncodeVarint overloads (uint64_t, int64_t,
// uint32_t, int32_t) because int32_t is typedef'd to long, not int.
// This header adds a template overload that casts to uint64_t.

#pragma once

#include "absl/log/internal/proto.h"
#include <type_traits>

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace log_internal {

template <typename T,
          typename = std::enable_if_t<
              std::is_integral_v<T> &&
              !std::is_same_v<std::decay_t<T>, uint64_t> &&
              !std::is_same_v<std::decay_t<T>, int64_t> &&
              !std::is_same_v<std::decay_t<T>, uint32_t> &&
              !std::is_same_v<std::decay_t<T>, int32_t>>>
inline bool EncodeVarint(uint64_t tag, T value, absl::Span<char> *buf) {
  return EncodeVarint(tag, static_cast<uint64_t>(value), buf);
}

}  // namespace log_internal
ABSL_NAMESPACE_END
}  // namespace absl
