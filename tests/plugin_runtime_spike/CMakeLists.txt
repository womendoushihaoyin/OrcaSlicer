cmake_minimum_required(VERSION 3.18)
project(OrcaLocalPluginSpike LANGUAGES CXX)
include(../../cmake/OrcaPluginPython.cmake)
enable_testing()
add_executable(orca_local_plugin_spike main.cpp)
target_link_libraries(orca_local_plugin_spike PRIVATE orca_local_plugin_runtime)
foreach(scenario happy exception duplicate dependency on_load_failure)
    add_test(NAME plugin_spike_${scenario}
        COMMAND orca_local_plugin_spike "${ORCA_PLUGIN_PYTHON_ROOT}" "${scenario}")
endforeach()
add_executable(orca_local_plugin_store_test store.cpp)
target_link_libraries(orca_local_plugin_store_test PRIVATE orca_local_plugin_runtime)
foreach(scenario transaction state boundaries lifecycle)
    add_test(NAME plugin_store_${scenario}
        COMMAND orca_local_plugin_store_test "${ORCA_PLUGIN_PYTHON_ROOT}" "${scenario}")
endforeach()
add_test(NAME plugin_store_process_restart
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/restart.py"
        "$<TARGET_FILE:orca_local_plugin_store_test>" "${ORCA_PLUGIN_PYTHON_ROOT}")
