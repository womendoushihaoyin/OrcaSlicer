# Real local dynamic libraries; no account, device or network traffic is required.
foreach(_app_info_variant good no_version debug_mismatch)
    add_library(orca_plugin_app_info_fixture_${_app_info_variant} SHARED EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/tests/plugin_core/network_module_fixture.cpp")
    target_compile_features(orca_plugin_app_info_fixture_${_app_info_variant} PRIVATE cxx_std_17)
    # Match the existing native module ABI: unmangled symbol, C++ string return.
    # This intentional ABI warning applies only to these fixture libraries.
    target_compile_options(orca_plugin_app_info_fixture_${_app_info_variant} PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-return-type-c-linkage>)
    set_target_properties(orca_plugin_app_info_fixture_${_app_info_variant} PROPERTIES
        OUTPUT_NAME "app_info_${_app_info_variant}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin-app-info-fixtures"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin-app-info-fixtures")
endforeach()
target_compile_definitions(orca_plugin_app_info_fixture_no_version PRIVATE APP_INFO_FIXTURE_NO_VERSION)
target_compile_definitions(orca_plugin_app_info_fixture_debug_mismatch PRIVATE APP_INFO_FIXTURE_DEBUG_MISMATCH)

add_executable(orca_plugin_app_info_test EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/tests/plugin_core/app_info.cpp")
target_compile_features(orca_plugin_app_info_test PRIVATE cxx_std_17)
target_link_libraries(orca_plugin_app_info_test PRIVATE libslic3r_gui pybind11::embed)
add_dependencies(orca_plugin_app_info_test orca_plugin_app_info_fixture_good
                 orca_plugin_app_info_fixture_no_version orca_plugin_app_info_fixture_debug_mismatch)
# Run with two UTF-8 arguments: a fresh disposable profile root and the fixture
# library directory ($<TARGET_FILE_DIR:orca_plugin_app_info_fixture_good>).
