# Include from OrcaPluginCore.cmake after libslic3r_gui is configured.
# Shares the production bridge and adapters; no duplicate/fake Python API.
add_executable(orca_plugin_sdk_export EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tools/plugin_sdk/main.cpp"
    "${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp"
    "${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp"
    "${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp"
)
target_link_libraries(orca_plugin_sdk_export PRIVATE libslic3r_gui pybind11::embed)
target_compile_features(orca_plugin_sdk_export PRIVATE cxx_std_17)
target_compile_definitions(orca_plugin_sdk_export PRIVATE PYBIND11_SIMPLE_GIL_MANAGEMENT)
