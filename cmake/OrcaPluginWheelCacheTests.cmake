# Real wheel archives, PluginManager imports, and Script callbacks in isolated
# processes. The Python driver preserves original bad archives and all outcomes.
add_executable(orca_plugin_wheel_cache_recovery_test EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/wheel_cache_recovery.cpp"
    "${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp"
    "${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp"
    "${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp")
target_link_libraries(orca_plugin_wheel_cache_recovery_test PRIVATE libslic3r_gui pybind11::embed)
target_compile_definitions(orca_plugin_wheel_cache_recovery_test PRIVATE PYBIND11_SIMPLE_GIL_MANAGEMENT)
target_compile_features(orca_plugin_wheel_cache_recovery_test PRIVATE cxx_std_17)
