# Real wxAppConsole pending-event queue; no GUI/profile or mocked dispatch required.
add_executable(orca_plugin_observer_queue_test EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/observer_queue.cpp"
    "${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp"
    "${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp"
    "${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp"
)
target_link_libraries(orca_plugin_observer_queue_test PRIVATE libslic3r_gui pybind11::embed)
target_compile_features(orca_plugin_observer_queue_test PRIVATE cxx_std_17)
target_compile_definitions(orca_plugin_observer_queue_test PRIVATE PYBIND11_SIMPLE_GIL_MANAGEMENT)

# Each scenario runs in a fresh process/profile and owns a real wx event queue.
add_executable(orca_plugin_unload_overlap_test EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/unload_overlap.cpp"
    "${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp"
    "${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp"
    "${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp"
)
target_link_libraries(orca_plugin_unload_overlap_test PRIVATE libslic3r_gui pybind11::embed)
target_compile_features(orca_plugin_unload_overlap_test PRIVATE cxx_std_17)
target_compile_definitions(orca_plugin_unload_overlap_test PRIVATE PYBIND11_SIMPLE_GIL_MANAGEMENT)

# Runtime::load exercises real no-app and wxAppConsole paths, without GUI windows.
add_executable(orca_plugin_runtime_wait_test EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/runtime_wait.cpp"
    "${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp"
    "${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp"
    "${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp"
)
target_link_libraries(orca_plugin_runtime_wait_test PRIVATE libslic3r_gui pybind11::embed)
target_compile_features(orca_plugin_runtime_wait_test PRIVATE cxx_std_17)
target_compile_definitions(orca_plugin_runtime_wait_test PRIVATE PYBIND11_SIMPLE_GIL_MANAGEMENT)
