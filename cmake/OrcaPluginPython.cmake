# Incremental local-plugin migration. Dependencies are fetched into the build
# tree; existing vendored snapshots and the user's system Python stay untouched.
include(FetchContent)

set(ORCA_PLUGIN_PYTHON_ROOT "" CACHE PATH "Explicit CPython 3.12.13 development/runtime prefix")
if(NOT ORCA_PLUGIN_PYTHON_ROOT)
    message(FATAL_ERROR "Local plugin development requires -DORCA_PLUGIN_PYTHON_ROOT=<CPython 3.12.13 prefix>")
endif()
set(Python3_ROOT_DIR "${ORCA_PLUGIN_PYTHON_ROOT}")
set(Python3_FIND_STRATEGY LOCATION)
set(Python3_FIND_REGISTRY NEVER)
set(Python3_FIND_FRAMEWORK NEVER)
if(WIN32)
    set(Python3_EXECUTABLE "${ORCA_PLUGIN_PYTHON_ROOT}/python.exe")
else()
    set(Python3_EXECUTABLE "${ORCA_PLUGIN_PYTHON_ROOT}/bin/python3.12")
endif()
find_package(Python3 3.12.13 EXACT REQUIRED COMPONENTS Interpreter Development.Embed)
get_filename_component(_orca_python_root "${ORCA_PLUGIN_PYTHON_ROOT}" REALPATH)
foreach(_orca_python_dependency IN LISTS Python3_EXECUTABLE Python3_INCLUDE_DIRS Python3_LIBRARIES)
    get_filename_component(_orca_python_resolved "${_orca_python_dependency}" REALPATH)
    string(FIND "${_orca_python_resolved}" "${_orca_python_root}/" _orca_python_prefix)
    if(NOT _orca_python_prefix EQUAL 0)
        message(FATAL_ERROR "Python resolver escaped ORCA_PLUGIN_PYTHON_ROOT: ${_orca_python_dependency}")
    endif()
endforeach()

set(PYBIND11_FINDPYTHON ON)
set(PYBIND11_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(orca_pybind11
    URL https://github.com/pybind/pybind11/archive/refs/tags/v3.0.1.tar.gz
    URL_HASH SHA256=741633da746b7c738bb71f1854f957b9da660bcd2dce68d71949037f0969d0ca
)
FetchContent_GetProperties(orca_pybind11)
if(NOT orca_pybind11_POPULATED)
    FetchContent_Populate(orca_pybind11)
    add_subdirectory("${orca_pybind11_SOURCE_DIR}" "${orca_pybind11_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()

add_library(orca_local_plugin_runtime STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../src/slic3r/plugin/LocalPluginRuntime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/slic3r/plugin/LocalPluginStore.cpp"
)
target_compile_features(orca_local_plugin_runtime PUBLIC cxx_std_17)
target_include_directories(orca_local_plugin_runtime PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../src"
    "${CMAKE_CURRENT_LIST_DIR}/../deps_src"
)
target_link_libraries(orca_local_plugin_runtime PUBLIC pybind11::embed)
target_compile_definitions(orca_local_plugin_runtime PUBLIC PYBIND11_SIMPLE_GIL_MANAGEMENT)


set(ORCA_PLUGIN_UV_EXECUTABLE "" CACHE FILEPATH "Pinned uv executable for the plugin dependency installer")
