add_executable(orca_plugin_precise_seam_test EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/precise_seam.cpp"
    "${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp"
    "${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp"
    "${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp"
)
target_link_libraries(orca_plugin_precise_seam_test PRIVATE libslic3r_gui pybind11::embed)
target_compile_features(orca_plugin_precise_seam_test PRIVATE cxx_std_17)
target_compile_definitions(orca_plugin_precise_seam_test PRIVATE PYBIND11_SIMPLE_GIL_MANAGEMENT)
