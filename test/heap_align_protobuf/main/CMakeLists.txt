set(OTEL_COMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../")

idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS
                        "${OTEL_COMP_DIR}/third_party/protobuf/src"
                        "${OTEL_COMP_DIR}/third_party/abseil-cpp"
                    REQUIRES esp-opentelemetry-cpp)
