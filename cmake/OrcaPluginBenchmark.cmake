# Standalone stage measurements. No wxApp/WebView is created, and this target
# must never be described as a full GUI startup benchmark.
add_executable(orca_plugin_benchmark EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tests/plugin_benchmark/main.cpp"
    "${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp"
    "${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp"
    "${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp"
)
target_link_libraries(orca_plugin_benchmark PRIVATE libslic3r_gui pybind11::embed)
target_compile_definitions(orca_plugin_benchmark PRIVATE PYBIND11_SIMPLE_GIL_MANAGEMENT)
target_compile_features(orca_plugin_benchmark PRIVATE cxx_std_17)

# Separate real-NumPy bulk and bounded retained-memory fixture. Existing samples
# and the original benchmark target remain unchanged.
add_executable(orca_plugin_bulk_benchmark EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tests/plugin_benchmark/bulk_main.cpp"
    "${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp"
    "${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp"
    "${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp"
)
target_link_libraries(orca_plugin_bulk_benchmark PRIVATE libslic3r_gui pybind11::embed)
target_compile_definitions(orca_plugin_bulk_benchmark PRIVATE PYBIND11_SIMPLE_GIL_MANAGEMENT)
target_compile_features(orca_plugin_bulk_benchmark PRIVATE cxx_std_17)
if(WIN32)
    target_link_libraries(orca_plugin_bulk_benchmark PRIVATE Psapi)
endif()
