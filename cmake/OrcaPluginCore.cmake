# The embedded bindings can take the GUI archive over 4 GiB on macOS.
# Apple ar/ranlib's 32-bit TOC asserts at that boundary; libtool explicitly
# emits the supported 64-bit TOC. This stays scoped to the GUI subdirectory.
if(APPLE)
    find_program(ORCA_APPLE_LIBTOOL NAMES libtool REQUIRED)
    set(CMAKE_CXX_CREATE_STATIC_LIBRARY
        "\"${ORCA_APPLE_LIBTOOL}\" -static -toc64 -D -o <TARGET> <LINK_FLAGS> <OBJECTS>")
endif()

# Fixed upstream implementation with explicit release_2.4.1 host adapters.
set(_plugin_core_sources
    PluginManager.cpp PluginLoader.cpp PluginInstallTransaction.cpp PluginDependencyInstaller.cpp PluginConfig.cpp PluginFsUtils.cpp
    PluginSlicing.cpp
    host/PluginResourceTextures.cpp
    PluginAuditManager.cpp PythonInterpreter.cpp PythonPluginBridge.cpp
    LocalPluginRuntimeCore.cpp LocalPluginStoreCore.cpp
    pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.cpp
    pluginTypes/script/ScriptPluginCapability.cpp
    pluginTypes/pages/PagesPluginCapability.cpp
    pluginTypes/resourceTexture/ResourceTexturePluginCapability.cpp
    host/PluginHostUi.cpp host/PluginConfigLocal.cpp host/PluginDockLocal.cpp host/PluginPagesLocal.cpp host/PluginHostCore.cpp host/PluginHostApp.cpp host/PluginHostPresets.cpp host/PluginHostGeometry.cpp host/PluginHostMesh.cpp host/PluginHostModel.cpp host/PluginHostSlicing.cpp
)
foreach(_source IN LISTS _plugin_core_sources)
    target_sources(libslic3r_gui PRIVATE "plugin/${_source}")
endforeach()
target_link_libraries(libslic3r_gui pybind11::embed)
target_compile_definitions(libslic3r_gui PRIVATE
    ORCA_PYTHON_PLUGINS PYBIND11_SIMPLE_GIL_MANAGEMENT
    ORCA_BUNDLED_PYTHON_ROOT="${ORCA_PLUGIN_PYTHON_ROOT}"
    ORCA_BUNDLED_UV_EXECUTABLE="${ORCA_PLUGIN_UV_EXECUTABLE}"
)
add_executable(orca_plugin_core_test
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/main.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/slicing_stages.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/slicing_plugin.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/gcode_plugin.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/path3.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/path3_projection.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_plugin.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_layers.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_model.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_kernel.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_obj.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_editor.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_color_editor.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_history.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_cut.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_layout_tools.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_image_adjust.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_island_groups.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_authoring.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_vertex_indices.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_seam_subdivision.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_edge_origins.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_visibility.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_mix_recipes.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/texture_boundaries.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/dependency_environment.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/contour.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/project_dirty.cpp"
    "${CMAKE_SOURCE_DIR}/tests/plugin_core/config_notify.cpp"
    "${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp"
    "${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp"
    "${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp"
)
target_link_libraries(orca_plugin_core_test PRIVATE libslic3r_gui pybind11::embed)
target_compile_definitions(orca_plugin_core_test PRIVATE PYBIND11_SIMPLE_GIL_MANAGEMENT)

target_sources(libslic3r_gui PRIVATE
    GUI/DockPanel.cpp GUI/WebDialog.cpp GUI/WebPanel.cpp GUI/PluginProgressDialog.cpp GUI/PluginSlicingGui.cpp GUI/PluginWorkflowSmoke.cpp
    ${CMAKE_SOURCE_DIR}/tests/plugin_core/gui_texture_mix.cpp
    GUI/Widgets/WebHosting.cpp GUI/Widgets/WebViewHostDialog.cpp
)

add_executable(orca_plugin_lock_test "${CMAKE_SOURCE_DIR}/tests/plugin_core/file_lock.cpp")
target_include_directories(orca_plugin_lock_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_features(orca_plugin_lock_test PRIVATE cxx_std_17)

include("${CMAKE_SOURCE_DIR}/cmake/OrcaPluginSdk.cmake")

# Local texture consumers retain the upstream bake pipeline; no Python in the kernel.
target_sources(libslic3r PRIVATE
    "${CMAKE_SOURCE_DIR}/src/libslic3r/TextureDisplacementImage.cpp"
    "${CMAKE_SOURCE_DIR}/src/libslic3r/TextureDisplacementConsumer.cpp"
    "${CMAKE_SOURCE_DIR}/src/libslic3r/TextureDisplacement.cpp"
)
target_sources(libslic3r_cgal PRIVATE "${CMAKE_SOURCE_DIR}/src/libslic3r/TextureDisplacementParameterize.cpp")
foreach(_texture_stage Mesh Index Displace Subdivide Regularize Relocate Flip Decimate Repair Debug Pipeline)
    target_sources(libslic3r PRIVATE "${CMAKE_SOURCE_DIR}/src/libslic3r/TextureBake/TextureBake${_texture_stage}.cpp")
endforeach()
target_sources(libslic3r_gui PRIVATE
    GUI/TextureLibrary.cpp GUI/TextureDisplacementDialog.cpp
    GUI/TextureEditSession.cpp GUI/EditableTextureDialog.cpp GUI/EditableTextureUV.cpp GUI/TextureColorBake.cpp
    GUI/TextureProjectorFrame.cpp
)

include("${CMAKE_SOURCE_DIR}/cmake/OrcaPluginPresetTests.cmake")

include("${CMAKE_SOURCE_DIR}/cmake/OrcaPluginPreciseSeamTests.cmake")

include("${CMAKE_SOURCE_DIR}/cmake/OrcaPluginObserverTests.cmake")
target_sources(libslic3r_gui PRIVATE "${CMAKE_SOURCE_DIR}/tests/plugin_core/gui_dock_teardown.cpp")
target_sources(libslic3r_gui PRIVATE "${CMAKE_SOURCE_DIR}/tests/plugin_core/gui_pages_restart.cpp"
        "${CMAKE_SOURCE_DIR}/tests/plugin_core/gui_dock_restart.cpp"
        "${CMAKE_SOURCE_DIR}/tests/plugin_core/gui_dualhost.cpp"
        "${CMAKE_SOURCE_DIR}/tests/plugin_core/gui_legacy_states.cpp")

include("${CMAKE_SOURCE_DIR}/cmake/OrcaPluginPrintObjectIdTests.cmake")

include("${CMAKE_SOURCE_DIR}/cmake/OrcaPluginBenchmark.cmake")

include("${CMAKE_SOURCE_DIR}/cmake/OrcaPluginAppInfoTests.cmake")

include("${CMAKE_SOURCE_DIR}/cmake/OrcaPluginDependencyRecoveryTests.cmake")

include("${CMAKE_SOURCE_DIR}/cmake/OrcaPluginWheelCacheTests.cmake")
