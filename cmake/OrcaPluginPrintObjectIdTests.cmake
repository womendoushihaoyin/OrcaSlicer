# Allocator poisoning is isolated to this executable, never the shared core runner.
add_executable(orca_plugin_print_object_id_test EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/print_object_id.cpp"
    "${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp"
    "${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp"
    "${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp"
)
target_link_libraries(orca_plugin_print_object_id_test PRIVATE libslic3r_gui pybind11::embed)
target_compile_features(orca_plugin_print_object_id_test PRIVATE cxx_std_17)
