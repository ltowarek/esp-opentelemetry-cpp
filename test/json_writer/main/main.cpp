// The json_generator JsonWriter writes each token's exact bytes and rejects malformed token streams.

#include "esp_json_writer.hpp"

extern "C" {
#include "unity.h"
}

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace {

using esp_opentelemetry::JsonWriter;

template <typename Write>
bool writes_ok(Write write) {
  const std::unique_ptr<JsonWriter> writer = esp_opentelemetry::MakeJsonGeneratorJsonWriter();
  write(*writer);
  return writer->ok();
}

template <typename Write>
std::string written(Write write) {
  const std::unique_ptr<JsonWriter> writer = esp_opentelemetry::MakeJsonGeneratorJsonWriter();
  write(*writer);
  return writer->ToString();
}

void test_empty_object() {
  TEST_ASSERT_EQUAL_STRING("{}", written([](JsonWriter& w) {
                             w.BeginObject();
                             w.EndObject();
                           }).c_str());
}

void test_keyed_strings_are_comma_separated() {
  TEST_ASSERT_EQUAL_STRING("{\"a\":\"x\",\"b\":\"y\"}", written([](JsonWriter& w) {
                             w.BeginObject();
                             w.Key("a");
                             w.WriteString("x");
                             w.Key("b");
                             w.WriteString("y");
                             w.EndObject();
                           }).c_str());
}

void test_nested_containers() {
  TEST_ASSERT_EQUAL_STRING("{\"o\":{\"a\":[]},\"l\":[{},[],\"s\"]}", written([](JsonWriter& w) {
                             w.BeginObject();
                             w.Key("o");
                             w.BeginObject();
                             w.Key("a");
                             w.BeginArray();
                             w.EndArray();
                             w.EndObject();
                             w.Key("l");
                             w.BeginArray();
                             w.BeginObject();
                             w.EndObject();
                             w.BeginArray();
                             w.EndArray();
                             w.WriteString("s");
                             w.EndArray();
                             w.EndObject();
                           }).c_str());
}

void test_top_level_scalar() {
  TEST_ASSERT_EQUAL_STRING("\"s\"", written([](JsonWriter& w) { w.WriteString("s"); }).c_str());
}

// A key is a view that need not be NUL-terminated.
void test_key_is_bounded_by_its_view() {
  TEST_ASSERT_EQUAL_STRING("{\"ab\":\"x\"}", written([](JsonWriter& w) {
                             w.BeginObject();
                             w.Key(std::string_view("abc", 2));
                             w.WriteString(std::string_view("xyz", 1));
                             w.EndObject();
                           }).c_str());
}

void test_string_escapes() {
  TEST_ASSERT_EQUAL_STRING("\"q\\\"b\\\\n\\nt\\tc\\u0001\"",
                           written([](JsonWriter& w) { w.WriteString("q\"b\\n\nt\tc\x01"); }).c_str());
}

void test_key_escapes() {
  TEST_ASSERT_EQUAL_STRING("{\"k\\\"\\n\":null}", written([](JsonWriter& w) {
                             w.BeginObject();
                             w.Key("k\"\n");
                             w.WriteNull();
                             w.EndObject();
                           }).c_str());
}

void test_int32_min() {
  TEST_ASSERT_EQUAL_STRING("-2147483648",
                           written([](JsonWriter& w) { w.WriteInt32(INT32_MIN); }).c_str());
}

void test_uint32_max() {
  TEST_ASSERT_EQUAL_STRING("4294967295",
                           written([](JsonWriter& w) { w.WriteUInt32(UINT32_MAX); }).c_str());
}

void test_int64_min() {
  TEST_ASSERT_EQUAL_STRING("-9223372036854775808",
                           written([](JsonWriter& w) { w.WriteInt64(INT64_MIN); }).c_str());
}

void test_int64_max() {
  TEST_ASSERT_EQUAL_STRING("9223372036854775807",
                           written([](JsonWriter& w) { w.WriteInt64(INT64_MAX); }).c_str());
}

void test_uint64_in_array() {
  TEST_ASSERT_EQUAL_STRING("[7]", written([](JsonWriter& w) {
                             w.BeginArray();
                             w.WriteUInt64(7);
                             w.EndArray();
                           }).c_str());
}

void test_uint64_above_int64_in_object() {
  TEST_ASSERT_EQUAL_STRING("{\"u\":18446744073709551615,\"v\":1}", written([](JsonWriter& w) {
                             w.BeginObject();
                             w.Key("u");
                             w.WriteUInt64(UINT64_MAX);
                             w.Key("v");
                             w.WriteInt32(1);
                             w.EndObject();
                           }).c_str());
}

void test_uint64_above_int64_in_array_fails() {
  TEST_ASSERT_FALSE(writes_ok([](JsonWriter& w) {
    w.BeginArray();
    w.WriteUInt64(UINT64_MAX);
  }));
}

void test_fractional_double() {
  TEST_ASSERT_EQUAL_STRING("0.1", written([](JsonWriter& w) { w.WriteDouble(0.1); }).c_str());
}

void test_integral_double() {
  TEST_ASSERT_EQUAL_STRING("3", written([](JsonWriter& w) { w.WriteDouble(3.0); }).c_str());
}

void test_large_double() {
  TEST_ASSERT_EQUAL_STRING("1e+300", written([](JsonWriter& w) { w.WriteDouble(1e300); }).c_str());
}

void test_double_needing_17_digits() {
  TEST_ASSERT_EQUAL_STRING("0.30000000000000004",
                           written([](JsonWriter& w) { w.WriteDouble(0.1 + 0.2); }).c_str());
}

void test_nan_is_null() {
  TEST_ASSERT_EQUAL_STRING("null", written([](JsonWriter& w) { w.WriteDouble(NAN); }).c_str());
}

void test_infinity_is_null() {
  TEST_ASSERT_EQUAL_STRING("null",
                           written([](JsonWriter& w) { w.WriteDouble(INFINITY); }).c_str());
}

void test_bools() {
  TEST_ASSERT_EQUAL_STRING("[true,false]", written([](JsonWriter& w) {
                             w.BeginArray();
                             w.WriteBool(true);
                             w.WriteBool(false);
                             w.EndArray();
                           }).c_str());
}

void test_no_bytes() {
  TEST_ASSERT_EQUAL_STRING("\"\"", written([](JsonWriter& w) { w.WriteBytes(nullptr, 0); }).c_str());
}

void test_one_byte_pads_twice() {
  const std::uint8_t bytes[] = {0x00};
  TEST_ASSERT_EQUAL_STRING("\"AA==\"",
                           written([&](JsonWriter& w) { w.WriteBytes(bytes, sizeof(bytes)); }).c_str());
}

void test_two_bytes_pad_once() {
  const std::uint8_t bytes[] = {0x00, 0x01};
  TEST_ASSERT_EQUAL_STRING("\"AAE=\"",
                           written([&](JsonWriter& w) { w.WriteBytes(bytes, sizeof(bytes)); }).c_str());
}

void test_three_bytes_need_no_padding() {
  const std::uint8_t bytes[] = {0x00, 0x01, 0x02};
  TEST_ASSERT_EQUAL_STRING("\"AAEC\"",
                           written([&](JsonWriter& w) { w.WriteBytes(bytes, sizeof(bytes)); }).c_str());
}

void test_bytes_use_the_standard_alphabet() {
  const std::uint8_t bytes[] = {0x00, 0x01, 0x02, 0xff};
  TEST_ASSERT_EQUAL_STRING("\"AAEC/w==\"",
                           written([&](JsonWriter& w) { w.WriteBytes(bytes, sizeof(bytes)); }).c_str());
}

void test_whole_multibyte_characters_are_unchanged() {
  TEST_ASSERT_EQUAL_STRING("\"kamera_\xc5\x82\"",
                           written([](JsonWriter& w) { w.WriteString("kamera_\xc5\x82"); }).c_str());
}

void test_byte_that_is_not_utf8_becomes_a_replacement_character() {
  TEST_ASSERT_EQUAL_STRING("\"a\xef\xbf\xbd" "b\"",
                           written([](JsonWriter& w) { w.WriteString("a\xff" "b"); }).c_str());
}

// FreeRTOS truncates task names by bytes, which can cut a character in half.
void test_character_cut_at_the_end_is_dropped() {
  TEST_ASSERT_EQUAL_STRING("\"w\xc4\x85tek_\"",
                           written([](JsonWriter& w) { w.WriteString("w\xc4\x85tek_\xc5"); }).c_str());
}

void test_key_that_is_not_utf8_gets_a_replacement_character() {
  TEST_ASSERT_EQUAL_STRING("{\"k\xef\xbf\xbd\":null}", written([](JsonWriter& w) {
                             w.BeginObject();
                             w.Key("k\xc0");
                             w.WriteNull();
                             w.EndObject();
                           }).c_str());
}

// Longer than the chunk json_generator flushes from.
void test_document_spanning_many_chunks() {
  const std::string text(1000, 'x');
  TEST_ASSERT_EQUAL_STRING(("\"" + text + "\"").c_str(),
                           written([&](JsonWriter& w) { w.WriteString(text); }).c_str());
}

void test_well_formed_document_is_ok() {
  TEST_ASSERT_TRUE(writes_ok([](JsonWriter& w) {
    w.BeginObject();
    w.Key("a");
    w.WriteInt32(1);
    w.EndObject();
  }));
}

void test_key_at_top_level_fails() {
  TEST_ASSERT_FALSE(writes_ok([](JsonWriter& w) { w.Key("a"); }));
}

void test_key_in_array_fails() {
  TEST_ASSERT_FALSE(writes_ok([](JsonWriter& w) {
    w.BeginArray();
    w.Key("a");
  }));
}

void test_second_key_without_value_fails() {
  TEST_ASSERT_FALSE(writes_ok([](JsonWriter& w) {
    w.BeginObject();
    w.Key("a");
    w.Key("b");
  }));
}

void test_value_without_key_fails() {
  TEST_ASSERT_FALSE(writes_ok([](JsonWriter& w) {
    w.BeginObject();
    w.WriteInt32(1);
  }));
}

void test_mismatched_end_fails() {
  TEST_ASSERT_FALSE(writes_ok([](JsonWriter& w) {
    w.BeginObject();
    w.EndArray();
  }));
}

void test_end_without_begin_fails() {
  TEST_ASSERT_FALSE(writes_ok([](JsonWriter& w) { w.EndObject(); }));
}

void test_object_closed_after_a_dangling_key_fails() {
  TEST_ASSERT_FALSE(writes_ok([](JsonWriter& w) {
    w.BeginObject();
    w.Key("a");
    w.EndObject();
  }));
}

void test_second_top_level_value_fails() {
  TEST_ASSERT_FALSE(writes_ok([](JsonWriter& w) {
    w.WriteInt32(1);
    w.WriteInt32(2);
  }));
}

void test_failure_is_sticky() {
  TEST_ASSERT_FALSE(writes_ok([](JsonWriter& w) {
    w.EndArray();
    w.WriteInt32(1);
  }));
}

void test_failed_writer_returns_what_preceded_the_failure() {
  TEST_ASSERT_EQUAL_STRING("{\"a\":1", written([](JsonWriter& w) {
                             w.BeginObject();
                             w.Key("a");
                             w.WriteInt32(1);
                             w.EndArray();
                             w.Key("b");
                             w.WriteInt32(2);
                           }).c_str());
}

}  // namespace

void setUp() {}
void tearDown() {}

extern "C" void app_main() {
  UNITY_BEGIN();
  RUN_TEST(test_empty_object);
  RUN_TEST(test_keyed_strings_are_comma_separated);
  RUN_TEST(test_nested_containers);
  RUN_TEST(test_top_level_scalar);
  RUN_TEST(test_key_is_bounded_by_its_view);
  RUN_TEST(test_string_escapes);
  RUN_TEST(test_key_escapes);
  RUN_TEST(test_int32_min);
  RUN_TEST(test_uint32_max);
  RUN_TEST(test_int64_min);
  RUN_TEST(test_int64_max);
  RUN_TEST(test_uint64_in_array);
  RUN_TEST(test_uint64_above_int64_in_object);
  RUN_TEST(test_uint64_above_int64_in_array_fails);
  RUN_TEST(test_fractional_double);
  RUN_TEST(test_integral_double);
  RUN_TEST(test_large_double);
  RUN_TEST(test_double_needing_17_digits);
  RUN_TEST(test_nan_is_null);
  RUN_TEST(test_infinity_is_null);
  RUN_TEST(test_bools);
  RUN_TEST(test_no_bytes);
  RUN_TEST(test_one_byte_pads_twice);
  RUN_TEST(test_two_bytes_pad_once);
  RUN_TEST(test_three_bytes_need_no_padding);
  RUN_TEST(test_bytes_use_the_standard_alphabet);
  RUN_TEST(test_whole_multibyte_characters_are_unchanged);
  RUN_TEST(test_byte_that_is_not_utf8_becomes_a_replacement_character);
  RUN_TEST(test_character_cut_at_the_end_is_dropped);
  RUN_TEST(test_key_that_is_not_utf8_gets_a_replacement_character);
  RUN_TEST(test_document_spanning_many_chunks);
  RUN_TEST(test_well_formed_document_is_ok);
  RUN_TEST(test_key_at_top_level_fails);
  RUN_TEST(test_key_in_array_fails);
  RUN_TEST(test_second_key_without_value_fails);
  RUN_TEST(test_value_without_key_fails);
  RUN_TEST(test_mismatched_end_fails);
  RUN_TEST(test_end_without_begin_fails);
  RUN_TEST(test_object_closed_after_a_dangling_key_fails);
  RUN_TEST(test_second_top_level_value_fails);
  RUN_TEST(test_failure_is_sticky);
  RUN_TEST(test_failed_writer_returns_what_preceded_the_failure);
  UNITY_END();
}
