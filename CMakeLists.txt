# esp-opentelemetry-cpp — ESP-IDF component integrating opentelemetry-cpp
# with the ESP32 / Xtensa toolchain.
#
# The vendored SDK (third_party/opentelemetry-cpp) is only compiled when
# CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED, CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED
# or CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED is set, keeping the firmware
# image small on builds that need none of them. When all are disabled, the
# component still compiles and exposes the esp_opentelemetry.hpp API
# through the header-only opentelemetry-cpp api/ tree, and every tracer
# call routes to the SDK's built-in no-op provider.
#
# When enabled, the full C++ SDK including the OTLP/HTTP exporter is
# compiled in. Protobuf and Abseil are vendored as submodules under
# third_party/ and built as part of this component.

set(ESP_OPENTELEMETRY_SRCS
    src/integration/esp_git_ref.cpp
    src/integration/esp_logs.cpp
    src/integration/esp_metrics.cpp
    src/integration/esp_tracing.cpp
    src/workarounds/posix_shims.c
    src/workarounds/esp_heap_align.cpp
)

# The OTLP/HTTP exporters and the esp_http_client transport behind them are
# only compiled when that exporter family is selected; their headers come from
# the SDK's OTLP/HTTP targets, which are not on the include path otherwise.
if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP AND (CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED
   OR CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED OR CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED))
    list(APPEND ESP_OPENTELEMETRY_SRCS
        src/integration/esp_http_client_transport.cpp
        src/integration/esp_otlp_http_exporters.cpp)
endif()

# The JTAG sources reference esp_apptrace unconditionally; the app-trace
# component only compiles those symbols when the app-trace transport is
# selected, which the Kconfig option depends on. esp_jtag_channel.cpp is the
# shared writer behind all four signals, including profiles, whose exporter is
# hand-rolled and does not go through esp_jtag_exporters.cpp.
if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG)
    list(APPEND ESP_OPENTELEMETRY_SRCS
        src/integration/esp_jtag_channel.cpp
        src/integration/esp_jtag_exporters.cpp)
endif()

# Profiling's sources reference gptimer/cJSON/the app descriptor unconditionally
# (no internal #ifdef), so — like tracing/metrics above — they're only built
# when the signal is enabled; esp_profiling_stub.cpp provides the no-op public
# API otherwise so callers can call it unconditionally either way.
if(CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED)
    list(APPEND ESP_OPENTELEMETRY_SRCS
        src/integration/esp_profiles_document.cpp
        src/integration/esp_profiling.cpp
        src/integration/esp_task_span_slot.cpp)
else()
    list(APPEND ESP_OPENTELEMETRY_SRCS src/integration/esp_profiling_stub.cpp)
endif()

# The ProfilesExporter factories are transports for a JSON document and do not
# depend on the sampler, so they follow the exporter families rather than
# CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED: firmware with profiling off can
# still name one without failing to link.
list(APPEND ESP_OPENTELEMETRY_SRCS src/integration/esp_profiles_exporter.cpp)

# The opentelemetry-cpp api/ tree is header-only and compiles without the
# SDK. Expose it as part of the component's public include path so the
# esp_opentelemetry.hpp facade can use Tracer/Span types regardless of
# whether CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED is on. When tracing is
# off the API falls back to the built-in no-op provider.
set(ESP_OPENTELEMETRY_CPP_API_INCLUDE
    "${CMAKE_CURRENT_LIST_DIR}/third_party/opentelemetry-cpp/api/include")

# sdk/resource is also header-only (ResourceAttributes/Resource declarations
# only; Resource::Create() etc. live in the SDK library, only linked when a
# signal is enabled). esp_tracing.hpp/esp_metrics.hpp/esp_logs.hpp reference
# these types in their public signatures unconditionally, so this must be on
# the include path even when the SDK library itself isn't built.
set(ESP_OPENTELEMETRY_CPP_SDK_RESOURCE_INCLUDE
    "${CMAKE_CURRENT_LIST_DIR}/third_party/opentelemetry-cpp/sdk/include")

set(ESP_OPENTELEMETRY_REQUIRES
    esp_http_client
    pthread
    esp_system
)

# REQUIRES cannot be conditioned on CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED
# or CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG the way SRCS above is:
# ESP-IDF expands component requirements before sdkconfig is loaded, so a
# Kconfig-conditional REQUIRES silently misses these regardless of the config
# value — they stay unconditional (a harmless extra link-time dependency when
# profiling or the JTAG exporter is disabled; app_trace itself compiles to
# nothing unless its transport is selected).
list(APPEND ESP_OPENTELEMETRY_REQUIRES
    app_trace
    cjson
    esp_app_format
    esp_hw_support
    esp_driver_gptimer
    esp_timer)

idf_component_register(
    SRCS ${ESP_OPENTELEMETRY_SRCS}
    INCLUDE_DIRS "include" "${ESP_OPENTELEMETRY_CPP_API_INCLUDE}" "${ESP_OPENTELEMETRY_CPP_SDK_RESOURCE_INCLUDE}"
    PRIV_INCLUDE_DIRS "src/integration"
    REQUIRES ${ESP_OPENTELEMETRY_REQUIRES}
    LDFRAGMENTS "src/workarounds/pb_defaults.lf"
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
# _Znwj = operator new(unsigned int) — Itanium ABI mangled name on 32-bit
# targets (size_t == unsigned int on Xtensa). Forces esp_heap_align.cpp.o
# onto the link line before the linker encounters any C++ allocation site,
# so the conforming operator new wins over any weak default.
idf_build_set_property(LINK_OPTIONS "-Wl,--undefined=_Znwj" APPEND)
# protobuf reads __start_pb_defaults/__stop_pb_defaults, which a linker
# synthesises only for an output section of that name. src/workarounds/
# pb_defaults.lf merges the section into flash_rodata instead, so the pair is
# defined here as an empty range — see that file for why walking nothing is
# correct. Defined unconditionally: unreferenced in builds that never emit the
# section.
idf_build_set_property(LINK_OPTIONS "-Wl,--defsym=__start_pb_defaults=0" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--defsym=__stop_pb_defaults=0" APPEND)

if(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED OR CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED
   OR CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
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
    # The JTAG exporters reuse the OTLP file exporters' OTLP/JSON serialisation
    # with an app-trace appender in place of a file, so each of this
    # component's exporter families maps onto one of the SDK's. The rest of
    # this file assumes protobuf is materialised (protobuf_setup.cmake, the
    # int32_t override, the INTERFACE libprotobuf link), so OTLP/HTTP stays on
    # even for an ostream-only build; dropping protobuf when no OTLP exporter
    # is compiled is issue #38.
    if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG)
        set(WITH_OTLP_FILE    ON  CACHE BOOL "" FORCE)
    else()
        set(WITH_OTLP_FILE    OFF CACHE BOOL "" FORCE)
    endif()
    set(WITH_OTLP_HTTP        ON  CACHE BOOL "" FORCE)
    set(WITH_PROMETHEUS       OFF CACHE BOOL "" FORCE)
    set(WITH_ZIPKIN           OFF CACHE BOOL "" FORCE)
    set(WITH_ELASTICSEARCH    OFF CACHE BOOL "" FORCE)
    set(WITH_ETW              OFF CACHE BOOL "" FORCE)
    set(WITH_ABSEIL           OFF CACHE BOOL "" FORCE)   # use std::variant on C++17
    set(WITH_STL              "CXX17" CACHE STRING "" FORCE)
    set(WITH_OTLP_HTTP_COMPRESSION OFF CACHE BOOL "" FORCE)
    set(OPENTELEMETRY_INSTALL OFF CACHE BOOL "" FORCE)
    # curl cannot be cross-compiled to Xtensa cleanly (autoprobes for
    # hostent/off_t/recv/send all fail). We provide our own
    # esp_http_client-based HttpClient via
    # esp_opentelemetry::MakeEspHttpClient(), passed directly to
    # OtlpHttpExporter's HttpClient constructor overload
    # (open-telemetry/opentelemetry-cpp#4071), so curl is never needed.
    set(WITH_HTTP_CLIENT_CURL OFF CACHE BOOL "" FORCE)

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

    # The component's own C++ TUs pull opentelemetry-cpp + protobuf headers that
    # transitively include absl/base/internal/thread_identity.h, whose
    # unconditional lock-free static_assert fails on some ESP toolchain configs.
    # Prepend the dedicated absl_shadow include dir so our thread_identity.h
    # shadow wins over Abseil's own -I (see cmake/protobuf_setup.cmake).
    target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/src/workarounds/absl_shadow")

    # Each signal links its SDK plus the exporter for the selected transport;
    # the unselected transport's exporter is never built into the image.
    if(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
        target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_trace)
        if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)
            target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_exporter_otlp_http)
        endif()
        if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG)
            target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_exporter_otlp_file)
        endif()
        if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OSTREAM)
            target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_exporter_ostream_span)
        endif()
    endif()

    if(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)
        target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_metrics)
        if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)
            target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_exporter_otlp_http_metric)
        endif()
        if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG)
            target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_exporter_otlp_file_metric)
        endif()
        if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OSTREAM)
            target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_exporter_ostream_metrics)
        endif()
    endif()

    if(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
        target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_logs)
        if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)
            target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_exporter_otlp_http_log)
        endif()
        if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG)
            target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_exporter_otlp_file_log)
        endif()
        if(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OSTREAM)
            target_link_libraries(${COMPONENT_LIB} PUBLIC opentelemetry_exporter_ostream_logs)
        endif()
    endif()

    target_link_libraries(${COMPONENT_LIB}
        PUBLIC opentelemetry_common
        INTERFACE protobuf::libprotobuf)
endif()
