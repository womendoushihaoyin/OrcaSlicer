#include "PluginHostBindings.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <memory>
#include <stdexcept>
#include <wx/thread.h>

namespace py = pybind11;
namespace Slic3r {
namespace {
void require_main_thread(const char* name)
{
    if (!wxIsMainThread())
        throw std::runtime_error(std::string(name) + "() must be called from the UI thread");
}
GUI::GUI_App& current_app(const char* name)
{
    require_main_thread(name);
    auto* app = dynamic_cast<GUI::GUI_App*>(wxApp::GetInstance());
    if (!app)
        throw std::runtime_error("Snapmaker Orca application is not initialized");
    if (PluginManager::instance().is_shutting_down())
        throw std::runtime_error("Snapmaker Orca is closing");
    return *app;
}
GUI::Plater* current_plater()
{
    auto* plater = current_app("plater").plater();
    if (!plater)
        throw std::runtime_error("Plater is not available");
    return plater;
}
PresetBundle* current_preset_bundle()
{
    auto* bundle = current_app("preset_bundle").preset_bundle;
    if (!bundle)
        throw std::runtime_error("Preset bundle is not available");
    return bundle;
}
void export_3mf_copy(GUI::Plater& plater, const std::string& path)
{
    require_main_thread("export_3mf_copy");
    if (!boost::iends_with(path, ".3mf"))
        throw py::value_error("export_3mf_copy() needs a path ending in .3mf");
    if (!GUI::OpenGLManager::get_gl_info().is_detected())
        throw std::runtime_error("export_3mf_copy() needs the 3D view, which is not ready yet");
    if (PySys_Audit("open", "ssi", path.c_str(), "w", 0) < 0)
        throw py::error_already_set();
    if (!plater.export_3mf_copy(GUI::into_path(GUI::from_u8(path))))
        throw std::runtime_error("Failed to write " + path);
}
py::object gl_info()
{
    require_main_thread("gl_info");
    const auto& info = GUI::OpenGLManager::get_gl_info();
    if (!info.is_detected())
        return py::none();
    py::dict out;
    out["vendor"] = info.get_vendor();
    out["renderer"] = info.get_renderer();
    out["version"] = info.get_version();
    out["glsl_version"] = info.get_glsl_version();
    out["core_profile"] = info.is_core_profile();
    return std::move(out);
}
} // namespace
void host_bindings::register_app(py::module_& host)
{
    py::class_<GUI::Plater, std::unique_ptr<GUI::Plater, py::nodelete>>(host, "Plater")
        .def("model", [](GUI::Plater& plater) -> Model& {
            require_main_thread("model");
            return plater.model();
        }, py::return_value_policy::reference_internal)
        .def("is_project_dirty", [](const GUI::Plater& plater) {
            require_main_thread("is_project_dirty");
            return plater.is_project_dirty();
        })
        .def("is_presets_dirty", [](const GUI::Plater& plater) {
            require_main_thread("is_presets_dirty");
            return plater.is_presets_dirty();
        })
        .def("inside_snapshot_capture", [](GUI::Plater& plater) {
            require_main_thread("inside_snapshot_capture");
            return plater.inside_snapshot_capture();
        })
        .def("project_path", [](GUI::Plater& plater) {
            require_main_thread("project_path");
            return GUI::into_u8(plater.get_project_filename(".3mf"));
        })
        .def("export_3mf_copy", &export_3mf_copy, py::arg("path"),
             "Export the current project without changing its file name or saved state.");
    host.def("plater", &current_plater, py::return_value_policy::reference);
    host.def("model", []() -> Model& { return current_plater()->model(); }, py::return_value_policy::reference);
    host.def("preset_bundle", &current_preset_bundle, py::return_value_policy::reference);
    host.def("gl_info", &gl_info);
}
} // namespace Slic3r
