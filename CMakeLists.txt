# esp-opentelemetry-cpp — ESP-IDF component integrating opentelemetry-cpp
# with the ESP32 / Xtensa toolchain.
#
# The vendored SDK (third_party/opentelemetry-cpp) is only added to the
# build when CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED is set, keeping the
# firmware image small on builds that do not need tracing. When disabled,
# the component still compiles and exposes the esp_opentelemetry.hpp API
# through the header-only opentelemetry-cpp api/ tree, and every tracer
# call routes to the SDK's built-in no-op provider.
#
# When enabled, the full C++ SDK including the OTLP/HTTP exporter is
# compiled in. Protobuf and Abseil are vendored as submodules under
# third_party/ and built as part of this component.

set(ESP_OPENTELEMETRY_SRCS
    src/esp_opentelemetry.cpp
    src/propagation.cpp
    src/esp_posix_shims.c
)

if(CONFIG_ESP_OPENTELEMETRY_HEAP_ALIGN_WORKAROUND)
    list(APPEND ESP_OPENTELEMETRY_SRCS src/esp_heap_align.cpp)
endif()

# NOTE: esp_http_client_transport.cpp is intentionally *not* listed
# here. When CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED is on, it is
# compiled as the sole source of the vendored
# `opentelemetry_http_client_curl` target (see the set_property(...
# SOURCES ...) call below), so it sits on the opentelemetry-cpp side of
# the link graph instead of our component. This keeps the factory free
# functions in the same translation unit opentelemetry-cpp expects them
# in and avoids compiling the file twice.

# The opentelemetry-cpp api/ tree is header-only and compiles without the
# SDK. Expose it as part of the component's public include path so the
# esp_opentelemetry.hpp facade can use Tracer/Span types regardless of
# whether CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED is on. When tracing is
# off the API falls back to the built-in no-op provider.
set(ESP_OPENTELEMETRY_CPP_API_INCLUDE
    "${CMAKE_CURRENT_LIST_DIR}/third_party/opentelemetry-cpp/api/include")

set(ESP_OPENTELEMETRY_REQUIRES esp_http_client json pthread)

idf_component_register(
    SRCS ${ESP_OPENTELEMETRY_SRCS}
    INCLUDE_DIRS "include" "${ESP_OPENTELEMETRY_CPP_API_INCLUDE}"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES ${ESP_OPENTELEMETRY_REQUIRES}
)

# Force the linker to keep our POSIX shims (nanosleep, pthread_atfork)
# from esp_posix_shims.c. Nothing inside libesp-opentelemetry-cpp.a
# references these directly — Abseil does, but Abseil comes later on
# the link line, by which point the linker has already moved past
# libesp_opentelemetry.a and will satisfy the references from newlib's
# libnosys pthread.o instead, dragging its pthread_mutex_*/pthread_exit
# stubs in and colliding with ESP-IDF's real pthread component. `-u`
# marks the symbol undefined at link start, which pulls in
# esp_posix_shims.c.o before libnosys is ever consulted.
idf_build_set_property(LINK_OPTIONS "-Wl,--undefined=nanosleep" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--undefined=pthread_atfork" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=sysconf" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--undefined=__wrap_sysconf" APPEND)
# Pull in esp_heap_align.cpp.o even before the linker encounters a C++
# allocation site.  _Znwj is operator new(unsigned int) — the Itanium ABI
# mangled name on 32-bit targets where size_t == unsigned int (Xtensa, RISC-V).
if(CONFIG_ESP_OPENTELEMETRY_HEAP_ALIGN_WORKAROUND)
    idf_build_set_property(LINK_OPTIONS "-Wl,--undefined=_Znwj" APPEND)
endif()

if(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
    # opentelemetry-cpp's architecture autodetection does not understand
    # Xtensa; short-circuit it by exporting ARCH to the value it uses for
    # debug logging only. The SDK's actual codegen does not depend on this
    # string outside of a couple of diagnostic messages and an optional
    # ETW path we have disabled.
    set(ENV{ARCH} "xtensa")

    # Tune the vendored opentelemetry-cpp build for an embedded target
    # before pulling it in. These must be set *before* the
    # add_subdirectory() call so the SDK's option()/set() lines see them.
    set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
    set(WITH_EXAMPLES         OFF CACHE BOOL "" FORCE)
    set(WITH_FUNC_TESTS       OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARK        OFF CACHE BOOL "" FORCE)
    set(WITH_OTLP_GRPC        OFF CACHE BOOL "" FORCE)
    set(WITH_OTLP_HTTP        ON  CACHE BOOL "" FORCE)
    set(WITH_PROMETHEUS       OFF CACHE BOOL "" FORCE)
    set(WITH_ZIPKIN           OFF CACHE BOOL "" FORCE)
    set(WITH_ELASTICSEARCH    OFF CACHE BOOL "" FORCE)
    set(WITH_ETW              OFF CACHE BOOL "" FORCE)
    set(WITH_ABSEIL           OFF CACHE BOOL "" FORCE)   # use std::variant on C++17
    set(WITH_STL              "CXX17" CACHE STRING "" FORCE)
    set(WITH_OTLP_HTTP_COMPRESSION OFF CACHE BOOL "" FORCE)
    set(OPENTELEMETRY_INSTALL OFF CACHE BOOL "" FORCE)

    # Advertise a stub libcurl so opentelemetry-cpp's cmake/curl.cmake
    # takes the find_package path and skips FetchContent of curl
    # itself. curl cannot be cross-compiled to Xtensa cleanly
    # (autoprobes for hostent/off_t/recv/send all fail), and we never
    # link the curl-based HTTP transport anyway — our custom
    # esp_http_client-based HttpClient is installed at runtime.
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/shims")

    # WITH_OTLP_HTTP=ON unconditionally pulls curl in as the SDK's default
    # HttpClient backend. We provide our own esp_http_client-based
    # transport via esp_opentelemetry::MakeEspHttpClient(), so nothing
    # calls the curl path at runtime — but otel-cpp still drags curl into
    # the build graph, and curl's CMake insists on finding a TLS backend
    # (OpenSSL by default, which isn't present on the Xtensa sysroot).
    # Disable every TLS backend and let curl build without SSL. The
    # resulting libcurl is never linked into the final image because
    # HttpClientFactory::Create() is only referenced from OtlpHttpClient's
    # default constructor, which we never call.
    set(CURL_ENABLE_SSL     OFF CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL    OFF CACHE BOOL "" FORCE)
    set(CURL_USE_MBEDTLS    OFF CACHE BOOL "" FORCE)
    set(CURL_USE_BEARSSL    OFF CACHE BOOL "" FORCE)
    set(CURL_USE_WOLFSSL    OFF CACHE BOOL "" FORCE)
    set(CURL_USE_GNUTLS     OFF CACHE BOOL "" FORCE)
    set(CURL_USE_NSS        OFF CACHE BOOL "" FORCE)
    set(CURL_USE_SCHANNEL   OFF CACHE BOOL "" FORCE)
    set(CURL_USE_SECTRANSP  OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2    OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL     OFF CACHE BOOL "" FORCE)
    set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP   ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS  ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_NTLM   ON  CACHE BOOL "" FORCE)
    set(USE_NGHTTP2         OFF CACHE BOOL "" FORCE)
    set(HTTP_ONLY           ON  CACHE BOOL "" FORCE)

    # curl's CMake runs check_type_size() for off_t and curl_off_t.
    # These probes fail under the Xtensa cross-compile (no standalone
    # sysroot to link against), leaving SIZEOF_CURL_OFF_T undefined.
    # curl_setup.h then evaluates `#if (SIZEOF_CURL_OFF_T < 8)` as
    # `#if (0 < 8)` and errors out. Pre-populate the cache so the
    # probes are skipped. ESP-IDF's newlib has 32-bit off_t; curl_off_t
    # is always int64_t (8 bytes).
    set(SIZEOF_OFF_T      4 CACHE INTERNAL "Size of off_t on Xtensa/newlib")
    set(SIZEOF_CURL_OFF_T 8 CACHE INTERNAL "Size of curl_off_t (always int64_t)")

    # curl's CMake runs check_c_source_compiles for recv/send signatures;
    # these fail on Xtensa cross-compile. Pre-populate the results so
    # curl_setup.h finds HAVE_RECV/HAVE_SEND and defines sread/swrite.
    # ESP-IDF's lwIP provides BSD-compatible recv/send signatures.
    set(HAVE_RECV 1 CACHE INTERNAL "")
    set(RECV_TYPE_ARG1 "int"    CACHE INTERNAL "")
    set(RECV_TYPE_ARG2 "void *" CACHE INTERNAL "")
    set(RECV_TYPE_ARG3 "size_t" CACHE INTERNAL "")
    set(RECV_TYPE_ARG4 "int"    CACHE INTERNAL "")
    set(RECV_TYPE_RETV "ssize_t" CACHE INTERNAL "")
    set(HAVE_SEND 1 CACHE INTERNAL "")
    set(SEND_TYPE_ARG1 "int"          CACHE INTERNAL "")
    set(SEND_TYPE_ARG2 "const void *" CACHE INTERNAL "")
    set(SEND_TYPE_ARG3 "size_t"       CACHE INTERNAL "")
    set(SEND_TYPE_ARG4 "int"          CACHE INTERNAL "")
    set(SEND_TYPE_RETV "ssize_t"      CACHE INTERNAL "")

    # ESP-IDF's newlib/lwIP provides ssize_t (typedef int). curl's
    # check_type_size("ssize_t") fails on cross-compile, causing it to
    # emit `#define ssize_t long` in curl_config.h — which collides
    # with lwIP's `typedef int ssize_t`. Pre-populate the result.
    set(SIZEOF_SSIZE_T      4 CACHE INTERNAL "sizeof(ssize_t) on Xtensa/newlib")
    set(HAVE_SIZEOF_SSIZE_T 1 CACHE INTERNAL "ssize_t exists in ESP-IDF newlib")

    # Similarly pre-populate other type sizes that curl probes.
    set(SIZEOF_SIZE_T  4 CACHE INTERNAL "sizeof(size_t) on Xtensa")
    set(SIZEOF_LONG    4 CACHE INTERNAL "sizeof(long) on Xtensa")
    set(SIZEOF_INT     4 CACHE INTERNAL "sizeof(int) on Xtensa")
    set(SIZEOF_TIME_T  4 CACHE INTERNAL "sizeof(time_t) on Xtensa/newlib")

    # Materialize Abseil + libprotobuf + the synthesized Protobuf package
    # config *before* opentelemetry-cpp's CMake runs. Without this, the
    # SDK's cmake/protobuf.cmake would fall through to FetchContent and
    # create a duplicate libprotobuf target. The include is guarded
    # inside the .cmake file so calling it again is a no-op.
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/protobuf_setup.cmake)

    add_subdirectory(
        ${CMAKE_CURRENT_LIST_DIR}/third_party/opentelemetry-cpp
        ${CMAKE_BINARY_DIR}/_deps/opentelemetry-cpp-build
        EXCLUDE_FROM_ALL
    )

    # Same int32_t-as-long fix the protobuf setup applies to Abseil
    # and libprotobuf. opentelemetry-cpp instantiates RepeatedField<int>
    # / Set<int> the same way protobuf internals do, so pin its
    # int32_t to `int` for every vendored target.
    _esp_opentelemetry_apply_int_override(
        ${CMAKE_CURRENT_LIST_DIR}/third_party/opentelemetry-cpp)

    # Replace the sources of the stock `opentelemetry_http_client_curl`
    # target with our esp_http_client-backed factory. The curl sources
    # cannot be cross-compiled (libcurl is a stub), and the public
    # OtlpHttpExporter constructor only consumes an OtlpHttpClient
    # constructed via HttpClientFactory — so redirecting the factory
    # here gives the stock SDK a working transport on ESP-IDF without
    # touching any vendored file.
    if(TARGET opentelemetry_http_client_curl)
        set_property(TARGET opentelemetry_http_client_curl
            PROPERTY SOURCES
                "${CMAKE_CURRENT_LIST_DIR}/src/esp_http_client_transport.cpp")
        # Link against the ESP-IDF component target so all transitive
        # include dirs (esp_http_client, esp_event, esp_common, log,
        # freertos, lwip, ...) propagate automatically.
        target_link_libraries(opentelemetry_http_client_curl PRIVATE
            idf::esp_http_client)
        target_include_directories(opentelemetry_http_client_curl PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/src")
    endif()

    # Target-level .pb.cc files produced by opentelemetry-proto at
    # configure time use the generated google/protobuf/*.pb.h that
    # ship in the protobuf source tree.  They need to see our vendored
    # protobuf headers *and* our Abseil headers without the build system
    # having to re-derive the include paths per translation unit, so
    # route them through protobuf::libprotobuf (already built by
    # cmake/protobuf_setup.cmake).
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/third_party/protobuf/src"
        "${CMAKE_CURRENT_LIST_DIR}/third_party/abseil-cpp")

    target_link_libraries(${COMPONENT_LIB}
        PUBLIC
            opentelemetry_trace
            opentelemetry_exporter_otlp_http
            opentelemetry_common
        INTERFACE
            protobuf::libprotobuf
    )
endif()
