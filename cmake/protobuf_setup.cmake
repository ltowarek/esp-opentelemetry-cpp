# Shared setup that materializes the target-side libprotobuf + Abseil and
# the host-side protoc for code generation. Intended to be include()d from
# CMakeLists.txt before calling add_subdirectory(opentelemetry-cpp) (which
# resolves find_package(Protobuf) at configure time).
#
# The guard below makes the setup idempotent: the first include() does the
# real work; subsequent include()s short-circuit.

if(TARGET protobuf::libprotobuf)
    return()
endif()

# -----------------------------------------------------------------------------
# Neutralize Threads::Threads on the ESP-IDF cross-compile.
#
# Abseil, opentelemetry-cpp, and other vendored projects call
# `find_package(Threads)` and link `Threads::Threads` into their static
# libraries. On this toolchain CMake's FindThreads resolves to
# `-lpthread`, which pulls in newlib's libnosys pthread stubs and
# collides at link time with the real ESP-IDF pthread component
# (multiple definitions of pthread_mutex_*, pthread_exit, etc.).
#
# Pre-declare an empty IMPORTED target and mark Threads as already
# found so FindThreads short-circuits: ESP-IDF already provides all
# pthread symbols through its `pthread` component.
if(NOT TARGET Threads::Threads)
    add_library(Threads::Threads INTERFACE IMPORTED GLOBAL)
endif()
set(Threads_FOUND             TRUE     CACHE INTERNAL "")
set(CMAKE_THREAD_LIBS_INIT    ""       CACHE INTERNAL "")
set(CMAKE_HAVE_THREADS_LIBRARY 1       CACHE INTERNAL "")
set(CMAKE_USE_PTHREADS_INIT   1        CACHE INTERNAL "")
set(THREADS_PREFER_PTHREAD_FLAG FALSE  CACHE INTERNAL "")

set(PROTOBUF_THIRD_PARTY "${CMAKE_CURRENT_LIST_DIR}/../third_party")
set(PROTOBUF_SRC_DIR     "${PROTOBUF_THIRD_PARTY}/protobuf")
set(ABSL_SRC_DIR         "${PROTOBUF_THIRD_PARTY}/abseil-cpp")
set(PROTOBUF_VERSION     "34.1.0")

# -----------------------------------------------------------------------------
# 1. Host protoc build
# -----------------------------------------------------------------------------
set(HOST_BUILD_DIR   "${CMAKE_BINARY_DIR}/_deps/host_protobuf-build")
set(HOST_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/host_protobuf-install")
set(PROTOBUF_HOST_PROTOC "${HOST_INSTALL_DIR}/bin/protoc")

if(NOT EXISTS "${PROTOBUF_HOST_PROTOC}")
    message(STATUS "Building host protoc from ${PROTOBUF_SRC_DIR} (v${PROTOBUF_VERSION})")
    file(MAKE_DIRECTORY "${HOST_BUILD_DIR}")

    # Use only the host toolchain. We explicitly scrub the CMake toolchain
    # variables IDF exported for the Xtensa cross-compile; otherwise the
    # host configure would try to build protoc with the Xtensa GCC.
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S "${CMAKE_CURRENT_LIST_DIR}/host_build"
            -B "${HOST_BUILD_DIR}"
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${HOST_INSTALL_DIR}
            -DCMAKE_C_COMPILER=cc
            -DCMAKE_CXX_COMPILER=c++
            -DCMAKE_TOOLCHAIN_FILE=
            -DCMAKE_SYSTEM_NAME=
            -DCMAKE_SYSTEM_PROCESSOR=
            -DABSL_SRC=${ABSL_SRC_DIR}
            -DPROTOBUF_SRC=${PROTOBUF_SRC_DIR}
        RESULT_VARIABLE _host_cfg_rc
        OUTPUT_VARIABLE _host_cfg_out
        ERROR_VARIABLE  _host_cfg_err
    )
    if(NOT _host_cfg_rc EQUAL 0)
        message(FATAL_ERROR
            "Host protoc configure failed (rc=${_host_cfg_rc}):\n"
            "STDOUT:\n${_host_cfg_out}\n"
            "STDERR:\n${_host_cfg_err}")
    endif()

    # Cap parallelism. A full libprotobuf + Abseil build + linking protoc
    # runs into tens of GB of RSS if every core is spawned; we have seen
    # host freezes (mouse unresponsive, clock stopped) on 4-core/7.7 GB
    # dev hosts with stock swap. Three jobs keeps the peak working set
    # below the OOM threshold while still using most of the CPU.
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build "${HOST_BUILD_DIR}"
                                 --target install
                                 --config Release
                                 --parallel 3
        RESULT_VARIABLE _host_build_rc
    )
    if(NOT _host_build_rc EQUAL 0)
        message(FATAL_ERROR "Host protoc build failed (rc=${_host_build_rc})")
    endif()

    if(NOT EXISTS "${PROTOBUF_HOST_PROTOC}")
        message(FATAL_ERROR
            "Host protoc build reported success but ${PROTOBUF_HOST_PROTOC} is missing")
    endif()
    message(STATUS "Host protoc built at ${PROTOBUF_HOST_PROTOC}")
endif()

# -----------------------------------------------------------------------------
# 2. Target (Xtensa) libprotobuf + Abseil
# -----------------------------------------------------------------------------
# opentelemetry-cpp and Abseil both look at CMAKE_SYSTEM_PROCESSOR.
# The IDF toolchain leaves it empty or set to "xtensa"; neither Abseil
# nor protobuf's generic branch requires a specific value, but a couple
# of probes (e.g. Abseil's RANDEN HW-AES selection) short-circuit to an
# empty-copts fallback for unknown processors — which is exactly what we
# want on Xtensa.

# Abseil: build the whole library (protobuf pulls ~30 targets in). None
# of the embedded-unfriendly features (cord, flags, status, log) are on
# our hot path at runtime, but they all have to compile because protobuf
# links them at descriptor init time.
# Abseil detects __XTENSA__ and assumes mmap/mprotect are available
# (ABSL_HAVE_MMAP). ESP-IDF's newlib does not ship <sys/mman.h>.
# Inject a stub header directory so the #include resolves to a no-op
# implementation that always returns MAP_FAILED.
set(ESP_OPENTELEMETRY_SHIMS_DIR "${CMAKE_CURRENT_LIST_DIR}/../shims")
include_directories(BEFORE SYSTEM "${ESP_OPENTELEMETRY_SHIMS_DIR}")

# Embedded-friendly Abseil defaults.
set(ABSL_PROPAGATE_CXX_STD       ON  CACHE BOOL "")
set(ABSL_ENABLE_INSTALL          OFF CACHE BOOL "")
set(ABSL_BUILD_TESTING           OFF CACHE BOOL "")
set(ABSL_USE_EXTERNAL_GOOGLETEST OFF CACHE BOOL "")
set(ABSL_BUILD_TEST_HELPERS      OFF CACHE BOOL "")
set(ABSL_FIND_GOOGLETEST         OFF CACHE BOOL "")

add_subdirectory(
    "${ABSL_SRC_DIR}"
    "${CMAKE_BINARY_DIR}/_deps/abseil-cpp-target-build"
    EXCLUDE_FROM_ALL
)

# Xtensa GCC defines __INT32_TYPE__ as `long int` (not `int`), so
# newlib typedefs int32_t to long. Upstream Abseil/protobuf/opentelemetry
# templates repeatedly assume int32_t == int (e.g. RepeatedField<int>
# instantiations, CppTypeFor<int> matches, std::variant<..., int32_t, ...>
# alternatives). Rather than patching every submodule source file,
# override the builtin __INT32_TYPE__ / __UINT32_TYPE__ for every
# translation unit compiled under the vendored trees. This makes
# int32_t a typedef for int within those TUs only, letting the upstream
# source compile unmodified. ESP-IDF components remain unaffected
# because they do not link against these internal typedefs.
#
# NOTE: Must be applied to every target created by add_subdirectory().
# CMake has no "directory-wide compile-definitions for subdir targets"
# primitive, so we walk BUILDSYSTEM_TARGETS recursively.
function(_esp_opentelemetry_apply_int_override dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "STATIC_LIBRARY" OR _type STREQUAL "OBJECT_LIBRARY"
           OR _type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "MODULE_LIBRARY"
           OR _type STREQUAL "EXECUTABLE")
            target_compile_options(${_t} PRIVATE
                -U__INT32_TYPE__ "-D__INT32_TYPE__=int"
                -U__UINT32_TYPE__ "-D__UINT32_TYPE__=unsigned int"
                # Neutralize ABSL_ATTRIBUTE_SECTION_VARIABLE: protobuf's
                # generated .pb.cc files mark descriptor tables with
                # __attribute__((section("protodesc_cold"))), which
                # lands data in a custom section that ESP-IDF's
                # sections.ld doesn't account for, breaking the
                # .flash.appdesc/.flash.rodata contiguous-layout
                # assertion. Folding those tables back into default
                # .rodata via an empty macro definition fixes it
                # without patching any submodule source.
                "SHELL:-D ABSL_ATTRIBUTE_SECTION_VARIABLE(name)="
                # Abseil's Mutex deadlock-detection (DebugOnlyDeadlockCheck)
                # calls GetOrCreateCurrentThreadIdentity -> pthread_key_create
                # -> pvTaskGetThreadLocalStoragePointer during protobuf's
                # global static constructors, which run before vTaskStartScheduler.
                # FreeRTOS TLS is not ready at that point -> LoadProhibited crash.
                # NDEBUG sets kMutexDeadlockDetectionMode = kIgnore, turning
                # DebugOnlyDeadlockCheck into a no-op. The uncontended lock
                # fast-paths via CAS and never touches TLS.
                -DNDEBUG)
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_s IN LISTS _subdirs)
        _esp_opentelemetry_apply_int_override("${_s}")
    endforeach()
endfunction()

_esp_opentelemetry_apply_int_override("${ABSL_SRC_DIR}")

# Abseil's log internals have ambiguous EncodeVarint overloads on
# 32-bit Xtensa GCC 13.2 where int/bool/pid_t do not match any of
# the four explicit overloads (int32_t is typedef'd to long, not
# int, on this platform). Force-include a shim that adds a template
# catch-all overload for the affected targets.
set(_ABSL_LOG_TARGETS
    absl_log_internal_structured_proto
    absl_log_internal_message
    absl_log_internal_log_sink_set)
foreach(_t IN LISTS _ABSL_LOG_TARGETS)
    if(TARGET ${_t})
        target_compile_options(${_t} PRIVATE
            -include "${ESP_OPENTELEMETRY_SHIMS_DIR}/absl_encode_varint_bool_fix.h")
    endif()
endforeach()

# Abseil's cctz time_zone_libc.cc depends on struct tm having
# tm_gmtoff/tm_zone members. ESP-IDF's newlib leaves these out
# unless __TM_GMTOFF / __TM_ZONE are pre-defined (see
# xtensa-esp-elf/include/time.h). Defining these macros *only*
# for the absl_time_zone TU adds those members to struct tm as
# seen by that TU. The TU only calls newlib's localtime_r, which
# writes the base fields; the extra members remain uninitialised,
# but this code path is never exercised at runtime on the device
# (Abseil timezone machinery isn't used — the OTLP exporter only
# emits Unix timestamps). The define is scoped to this one target
# to avoid ABI mismatches elsewhere.
if(TARGET absl_time_zone)
    target_compile_definitions(absl_time_zone PRIVATE
        __TM_GMTOFF=tm_gmtoff
        __TM_ZONE=tm_zone)
endif()

# Protobuf's CMake runs a try_compile for std::atomic<int64_t> and, on
# failure, sets protobuf_LINK_LIBATOMIC=true. The try_compile does fail
# under the Xtensa cross-compile (it links a standalone program without
# ESP-IDF's component framework). That wouldn't matter, except that
# cmake/protobuf-configure-target.cmake then hardcodes
# `target_link_libraries(libprotobuf PRIVATE atomic)` even when invoked
# from libprotobuf-lite.cmake — at which point the libprotobuf target
# has not been created yet, and CMake aborts. Upstream bug; avoid the
# path entirely by pre-seeding the cache. The ESP-IDF Xtensa toolchain
# implicitly links libatomic, so dropping the explicit link is safe.
set(protobuf_HAVE_BUILTIN_ATOMICS TRUE CACHE INTERNAL "Skip protobuf's atomic probe on Xtensa")

# Embedded-friendly protobuf defaults. FORCE the values that must
# override any earlier in-tree set() calls.
set(protobuf_BUILD_TESTS              OFF CACHE BOOL   "" FORCE)
set(protobuf_BUILD_EXAMPLES           OFF CACHE BOOL   "" FORCE)
set(protobuf_BUILD_CONFORMANCE        OFF CACHE BOOL   "" FORCE)
set(protobuf_BUILD_LIBUPB             OFF CACHE BOOL   "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES    OFF CACHE BOOL   "" FORCE)
set(protobuf_BUILD_PROTOBUF_BINARIES  ON  CACHE BOOL   "" FORCE)
set(protobuf_DISABLE_RTTI             OFF CACHE BOOL   "" FORCE)
set(protobuf_WITH_ZLIB                OFF CACHE BOOL   "" FORCE)
set(protobuf_INSTALL                  OFF CACHE BOOL   "" FORCE)
set(protobuf_ABSL_PROVIDER            "module" CACHE STRING "" FORCE)
set(protobuf_FORCE_FETCH_DEPENDENCIES OFF CACHE BOOL   "" FORCE)
set(protobuf_LOCAL_DEPENDENCIES_ONLY  ON  CACHE BOOL   "" FORCE)
add_subdirectory(
    "${PROTOBUF_SRC_DIR}"
    "${CMAKE_BINARY_DIR}/_deps/protobuf-target-build"
    EXCLUDE_FROM_ALL
)

_esp_opentelemetry_apply_int_override("${PROTOBUF_SRC_DIR}")

# IDF's Xtensa build sets -Werror=all. protobuf v34's arena code has
# several -Wmaybe-uninitialized false positives (compiler can't prove
# that a sequence of atomic CAS calls always assigns the variable).
# Downgrade to a warning for the vendored protobuf targets only.
foreach(_pb_target libprotobuf libprotobuf-lite utf8_range utf8_validity)
    if(TARGET ${_pb_target})
        target_compile_options(${_pb_target} PRIVATE
            -Wno-error=maybe-uninitialized
            -Wno-error=deprecated-declarations)
    endif()
endforeach()

# -----------------------------------------------------------------------------
# 3. Expose protoc to downstream find_package(Protobuf) calls
# -----------------------------------------------------------------------------
# protobuf's target-build does not create the protobuf::protoc imported
# target because protobuf_BUILD_PROTOC_BINARIES is OFF. opentelemetry-cpp
# consults $<TARGET_FILE:protobuf::protoc> or Protobuf_PROTOC_EXECUTABLE
# (cross-compile path). Provide both.
if(NOT TARGET protobuf::protoc)
    add_executable(protobuf::protoc IMPORTED GLOBAL)
    set_target_properties(protobuf::protoc PROPERTIES
        IMPORTED_LOCATION "${PROTOBUF_HOST_PROTOC}")
endif()

set(Protobuf_PROTOC_EXECUTABLE "${PROTOBUF_HOST_PROTOC}"
    CACHE FILEPATH "Path to host protoc binary" FORCE)
set(PROTOBUF_PROTOC_EXECUTABLE "${PROTOBUF_HOST_PROTOC}"
    CACHE FILEPATH "Path to host protoc binary" FORCE)

# Synthesize a package-config so downstream find_package(Protobuf CONFIG)
# calls succeed — no network, no apt. We intentionally do not rely on
# protobuf's own install step because that expands into a sprawling
# install tree we don't want committed to the build directory.
set(PROTOBUF_INCLUDE_DIR "${PROTOBUF_SRC_DIR}/src")
set(PROTOBUF_CONFIG_PREFIX "${CMAKE_BINARY_DIR}/_deps/esp_opentelemetry_protobuf_prefix")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/ProtobufConfig.cmake.in"
    "${PROTOBUF_CONFIG_PREFIX}/lib/cmake/protobuf/ProtobufConfig.cmake"
    @ONLY)

# Point find_package(Protobuf CONFIG) directly at our stub. Protobuf_DIR
# is checked before CMAKE_PREFIX_PATH, so this bypasses any ambiguity in
# filesystem-layout conventions (lib/cmake/protobuf vs Protobuf).
set(Protobuf_DIR "${PROTOBUF_CONFIG_PREFIX}/lib/cmake/protobuf"
    CACHE PATH "Directory containing ProtobufConfig.cmake" FORCE)
