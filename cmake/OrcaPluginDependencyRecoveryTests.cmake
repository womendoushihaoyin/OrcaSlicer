# A separate native process keeps dependency crash injection out of the app and
# ordinary tests. This fault injector uses macOS dyld/libc++; other OSes remain
# unverified until they have an equivalent real filesystem interruption runner.
if(APPLE)
    add_executable(orca_plugin_dependency_recovery_test EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/tests/plugin_core/dependency_recovery.cpp"
        "${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp"
        "${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp"
        "${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp")
    target_link_libraries(orca_plugin_dependency_recovery_test PRIVATE libslic3r_gui pybind11::embed)
    target_compile_definitions(orca_plugin_dependency_recovery_test PRIVATE PYBIND11_SIMPLE_GIL_MANAGEMENT)
    target_compile_features(orca_plugin_dependency_recovery_test PRIVATE cxx_std_17)
    add_library(orca_plugin_dependency_fault SHARED EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/tests/plugin_core/dependency_crash_interpose.cpp")
    target_compile_features(orca_plugin_dependency_fault PRIVATE cxx_std_17)
    add_dependencies(orca_plugin_dependency_recovery_test orca_plugin_dependency_fault)
endif()
