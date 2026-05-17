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
    src/integration/esp_opentelemetry.cpp
    src/workarounds/posix_shims.c
)

# NOTE: esp_http_client_transport.cpp is intentionally *not* listed
# here. When CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED is on, it is
# compiled into the opentelemetry_http_client_esp STATIC target below
# and PRIVATE-linked into opentelemetry_exporter_otlp_http_client so
# the HttpClientFactory symbols resolve at the right point on the link
# line without being compiled twice.

# The opentelemetry-cpp api/ tree is header-only and compiles without the
# SDK. Expose it as part of the component's public include path so the
# esp_opentelemetry.hpp facade can use Tracer/Span types regardless of
# whether CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED is on. When tracing is
# off the API falls back to the built-in no-op provider.
set(ESP_OPENTELEMETRY_CPP_API_INCLUDE
    "${CMAKE_CURRENT_LIST_DIR}/third_party/opentelemetry-cpp/api/include")

set(ESP_OPENTELEMETRY_REQUIRES esp_http_client pthread)

idf_component_register(
    SRCS ${ESP_OPENTELEMETRY_SRCS}
    INCLUDE_DIRS "include" "${ESP_OPENTELEMETRY_CPP_API_INCLUDE}"
    PRIV_INCLUDE_DIRS "src/integration"
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
    # Skip libcurl entirely; we supply HttpClientFactory via
    # opentelemetry_http_client_esp below.
    set(WITH_HTTP_CLIENT_CURL   OFF CACHE BOOL "" FORCE)

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

    # Provide the esp_http_client-backed HttpClientFactory that the OTLP HTTP
    # exporter calls through. WITH_HTTP_CLIENT_CURL=OFF leaves the exporter
    # without a linked factory; we supply one here via a named STATIC target
    # PRIVATE-linked into the exporter so the factory symbols sit in the right
    # place on the link line.
    add_library(opentelemetry_http_client_esp STATIC
        "${CMAKE_CURRENT_LIST_DIR}/src/integration/esp_http_client_transport.cpp")
    target_link_libraries(opentelemetry_http_client_esp PRIVATE
        idf::esp_http_client opentelemetry_ext opentelemetry_sdk)
    target_include_directories(opentelemetry_http_client_esp PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/src/integration")
    target_link_libraries(opentelemetry_exporter_otlp_http_client PRIVATE
        opentelemetry_http_client_esp)

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
