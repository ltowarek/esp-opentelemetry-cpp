# Stub FindCURL module for ESP-IDF / Xtensa builds.
#
# opentelemetry-cpp's cmake/curl.cmake requires a CURL::libcurl target
# when WITH_OTLP_HTTP=ON, because the SDK's default HTTP client
# (OtlpHttpClient) is implemented on top of libcurl. We replace that
# transport at runtime with an esp_http_client-based one registered
# through opentelemetry::ext::http::client::HttpClientFactory, so the
# curl code path is never reached — but opentelemetry-cpp still
# resolves the CMake dependency at configure time.
#
# Building real libcurl for Xtensa is possible but expensive and
# fragile (requires shims for struct hostent, off_t probes, TLS
# backend disablement, etc.). Instead we advertise a dummy
# CURL::libcurl that satisfies the CMake dependency. The
# opentelemetry_http_client_curl target's sources are then replaced
# with our esp_http_client-backed transport (see CMakeLists.txt).

set(CURL_FOUND TRUE)
set(CURL_VERSION "0.0.0-esp-opentelemetry-cpp-stub")
set(CURL_VERSION_STRING "${CURL_VERSION}")

if(NOT TARGET CURL::libcurl)
    add_library(CURL::libcurl INTERFACE IMPORTED GLOBAL)
endif()
