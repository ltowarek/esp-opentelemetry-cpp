idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS ""
                    REQUIRES esp-opentelemetry-cpp)

target_link_libraries(${COMPONENT_LIB} PRIVATE opentelemetry_exporter_ostream_span)
