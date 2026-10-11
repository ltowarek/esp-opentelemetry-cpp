cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(heap_align_cjson)

# Opt-in via CONFIG_HEAP_ALIGN_CJSON_UBSAN (sdkconfig.defaults.ubsan). Scoped to
# cJSON and main: the check must fire inside cJSON's own code, and project-wide
# UBSan would also instrument unrelated IDF components.
if(CONFIG_HEAP_ALIGN_CJSON_UBSAN)
    foreach(component espressif__cjson main)
        idf_component_get_property(lib ${component} COMPONENT_LIB)
        target_compile_options(${lib} PRIVATE "-fsanitize=undefined" "-fno-sanitize=shift-base")
    endforeach()
endif()
