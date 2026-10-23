#include "PluginHostBindings.hpp"
#include "PluginHostUi.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include <pybind11/pytypes.h>
#include <pybind11/cast.h>
#include <pybind11/pybind11.h>

#include <cstddef>
#include <libslic3r/AppConfig.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PresetBundle.hpp>

#include <memory>
#include <cstdint>
#include <mutex>
#include <tuple>
#include <wx/thread.h>

#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MainFrame.hpp>
#include <slic3r/GUI/Plater.hpp>
#include <slic3r/GUI/Tab.hpp>

#include <pybind11/stl.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace Slic3r {
namespace {

void require_preset_access()
{
    if (!wxIsMainThread())
        throw std::runtime_error("Preset access must run on the UI thread");
    if (PluginManager::instance().is_shutting_down())
        throw std::runtime_error("Preset access is unavailable while the application is closing");
}

// The deque's sorted slots are not stable identities: erase/insert/reload may
// move a different profile into an old address. Keep only the collection owner
// and the logical identity; resolve under its mutex for every property read.
struct PresetIdentity
{
    std::string name, file, bundle_id, setting_id;
    Preset::Type type;
    bool is_default, is_system, is_external, is_project_embedded;
    explicit PresetIdentity(const Preset& preset)
        : name(preset.name), file(preset.file), bundle_id(preset.bundle_id), setting_id(preset.setting_id), type(preset.type),
          is_default(preset.is_default), is_system(preset.is_system), is_external(preset.is_external),
          is_project_embedded(preset.is_project_embedded) {}
    bool matches(const Preset& preset) const
    {
        return name == preset.name && file == preset.file && bundle_id == preset.bundle_id && setting_id == preset.setting_id &&
               type == preset.type && is_default == preset.is_default && is_system == preset.is_system &&
               is_external == preset.is_external && is_project_embedded == preset.is_project_embedded;
    }
    py::tuple cache_key(const PresetCollection* collection, bool edited) const
    {
        return py::make_tuple(reinterpret_cast<std::uintptr_t>(collection), edited, name, file, bundle_id, setting_id,
                              int(type), is_default, is_system, is_external, is_project_embedded);
    }
};
struct PresetView
{
    py::object owner;
    PresetCollection* collection;
    PresetIdentity identity;
    bool edited;
    mutable bool valid = true;

    template<class Fn> auto read(Fn fn) const
    {
        require_preset_access();
        std::lock_guard<PresetCollection> lock(*collection);
        const Preset* current = nullptr;
        if (valid) {
            if (edited) {
                current = &collection->get_edited_preset();
                // A deleted selected profile can leave an old edited copy behind.
                if (std::none_of(collection->get_presets().begin(), collection->get_presets().end(),
                                 [&](const Preset& preset) { return identity.matches(preset); }))
                    current = nullptr;
            } else {
                for (const Preset& preset : collection->get_presets())
                    if (preset.name == identity.name) {
                        current = &preset;
                        break;
                    }
            }
        }
        if (!current || !identity.matches(*current)) {
            valid = false;
            throw std::runtime_error("Preset handle is no longer valid: " + identity.name + " was removed or replaced");
        }
        return fn(*current);
    }
};

// The cache is a Python WeakValueDictionary owned by module functions, not a
// process static. It preserves alias identity without retaining views or Python
// objects after interpreter shutdown.
py::object preset_view(const py::object& cache, py::object owner, PresetCollection& collection, const Preset* preset)
{
    if (!preset)
        return py::none();
    const bool edited = preset == &collection.get_edited_preset();
    const PresetIdentity identity(*preset);
    const auto key = identity.cache_key(&collection, edited);
    py::object existing = cache.attr("get")(key);
    if (!existing.is_none() && existing.cast<PresetView&>().valid)
        return existing;
    py::object view = py::cast(PresetView{std::move(owner), &collection, identity, edited});
    cache.attr("__setitem__")(key, view);
    return view;
}
py::object collection_owner(py::object bundle, PresetCollection& collection)
{
    return py::cast(&collection, py::return_value_policy::reference_internal, bundle);
}

DynamicPrintConfig full_config(const PresetBundle& bundle)
{
    require_preset_access();
    // release_2.4.1 indexes filament_presets[0] even for a freshly constructed
    // bundle. Do not let a plugin turn an uninitialized selection into a crash.
    if (bundle.printers.get_edited_preset().printer_technology() == ptFFF && bundle.filament_presets.empty())
        throw std::runtime_error("Filament preset selections are not initialized");
    return bundle.full_config();
}

PresetCollection& printer_presets(PresetBundle& bundle)
{
    require_preset_access();
    return static_cast<PresetCollection&>(bundle.printers);
}

void reload_local_bundle(const std::string& bundle_id)
{
    bool reloaded = false;
    std::string error;
    PluginHostUi::run_on_ui_thread([&] {
        auto* app = dynamic_cast<GUI::GUI_App*>(wxApp::GetInstance());
        if (!app || PluginManager::instance().is_shutting_down())
            throw std::runtime_error("Snapmaker Orca application is unavailable or closing");
        if (!app->preset_bundle || !app->app_config || !app->mainframe)
            throw std::runtime_error("Preset bundle and application configuration are not ready");
        reloaded = app->preset_bundle->reload_local_bundle(app->app_config->get("preset_folder"), bundle_id, &error);
        if (!reloaded)
            return;
        app->mainframe->update_side_preset_ui();
        for (auto* tab : app->tabs_list) {
            tab->reload_config();
            tab->update_changed_ui();
        }
        if (auto* plater = app->plater())
            plater->sidebar().update_all_preset_comboboxes();
    });
    if (!reloaded)
        throw std::runtime_error(error.empty() ? "Local bundle was not reloaded" : error);
}

std::vector<std::string> list_bundle_ids()
{
    std::vector<std::string> ids;
    PluginHostUi::run_on_ui_thread([&] {
        auto* app = dynamic_cast<GUI::GUI_App*>(wxApp::GetInstance());
        if (!app || PluginManager::instance().is_shutting_down())
            throw std::runtime_error("Snapmaker Orca application is unavailable or closing");
        if (!app->preset_bundle || !app->app_config)
            throw std::runtime_error("Preset bundle and application configuration are not ready");
        ids = app->preset_bundle->list_local_bundle_ids(app->app_config->get("preset_folder"));
    });
    return ids;
}

} // namespace

void host_bindings::register_presets(py::module_& host)
{
    auto preset = host.def_submodule("preset", "Preset operations");
    auto bundles = preset.def_submodule("bundles", "Local preset bundle operations");
    bundles.def("reload_local", &reload_local_bundle, py::arg("bundle_id"),
                "Reload a local bundle atomically, preserving modified selections.");
    bundles.def("list_ids", &list_bundle_ids,
                "List valid local bundle IDs in the active user's _local directory, including newly added bundles.");
    py::enum_<Preset::Type>(host, "PresetType")
        .value("Invalid", Preset::TYPE_INVALID)
        .value("Print", Preset::TYPE_PRINT)
        .value("SlaPrint", Preset::TYPE_SLA_PRINT)
        .value("Filament", Preset::TYPE_FILAMENT)
        .value("SlaMaterial", Preset::TYPE_SLA_MATERIAL)
        .value("Printer", Preset::TYPE_PRINTER)
        .value("PhysicalPrinter", Preset::TYPE_PHYSICAL_PRINTER)
        .value("Plate", Preset::TYPE_PLATE)
        .value("Model", Preset::TYPE_MODEL);

    const py::object cache = py::module_::import("weakref").attr("WeakValueDictionary")();
    auto field = [](auto member) {
        return [member](const PresetView& view) { return view.read([member](const Preset& preset) { return preset.*member; }); };
    };
    py::class_<PresetView>(host, "Preset")
        .def_property_readonly("type", field(&Preset::type))
        .def_property_readonly("name", field(&Preset::name))
        .def_property_readonly("alias", field(&Preset::alias))
        .def_property_readonly("file", field(&Preset::file))
        .def_property_readonly("is_default", field(&Preset::is_default))
        .def_property_readonly("is_external", field(&Preset::is_external))
        .def_property_readonly("is_system", field(&Preset::is_system))
        .def_property_readonly("is_visible", field(&Preset::is_visible))
        .def_property_readonly("is_dirty", field(&Preset::is_dirty))
        .def_property_readonly("is_compatible", field(&Preset::is_compatible))
        .def_property_readonly("is_project_embedded", field(&Preset::is_project_embedded))
        .def_property_readonly("bundle_id", field(&Preset::bundle_id))
        .def("is_user", [](const PresetView& view) { return view.read([](const Preset& preset) { return preset.is_user(); }); })
        .def("is_from_bundle", [](const PresetView& view) { return view.read([](const Preset& preset) { return preset.is_from_bundle(); }); })
        .def("label", [](const PresetView& view, bool no_alias) {
            return view.read([no_alias](const Preset& preset) { return preset.label(no_alias); });
        }, py::arg("no_alias") = false)
        .def("config_keys", [](const PresetView& view) { return view.read([](const Preset& preset) { return preset.config.keys(); }); })
        .def("config_value", [](const PresetView& view, const std::string& key) {
            return view.read([&key](const Preset& preset) { return config_value_or_none(preset.config, key); });
        });

    auto selected = [cache](py::object self) {
        require_preset_access();
        auto& collection = self.cast<PresetCollection&>();
        std::lock_guard<PresetCollection> lock(collection);
        return preset_view(cache, self, collection, &collection.get_selected_preset());
    };
    auto edited = [cache](py::object self) {
        require_preset_access();
        auto& collection = self.cast<PresetCollection&>();
        std::lock_guard<PresetCollection> lock(collection);
        return preset_view(cache, self, collection, &collection.get_edited_preset());
    };
    auto selected_name = [](PresetCollection& collection) {
        require_preset_access();
        std::lock_guard<PresetCollection> lock(collection);
        return collection.get_selected_preset_name();
    };
    py::class_<PresetCollection, std::unique_ptr<PresetCollection, py::nodelete>>(host, "PresetCollection")
        .def("size", [](PresetCollection& collection) {
            require_preset_access();
            std::lock_guard<PresetCollection> lock(collection);
            return collection.size();
        })
        .def("get_selected_preset", selected)
        .def("selected_preset", selected)
        .def("get_selected_preset_name", selected_name)
        .def("selected_preset_name", selected_name)
        .def("get_edited_preset", edited)
        .def("edited_preset", edited)
        .def("preset", [cache](py::object self, size_t index) {
            require_preset_access();
            auto& collection = self.cast<PresetCollection&>();
            std::lock_guard<PresetCollection> lock(collection);
            if (index >= collection.size())
                throw py::index_error("preset index out of range");
            return preset_view(cache, self, collection, &collection.preset(index));
        })
        .def("find_preset", [cache](py::object self, const std::string& name) {
            require_preset_access();
            auto& collection = self.cast<PresetCollection&>();
            std::lock_guard<PresetCollection> lock(collection);
            return preset_view(cache, self, collection, collection.find_preset(name));
        })
        .def("preset_names", [](PresetCollection& collection) {
            require_preset_access();
            std::lock_guard<PresetCollection> lock(collection);
            std::vector<std::string> names;
            names.reserve(collection.get_presets().size());
            for (const Preset& preset : collection.get_presets())
                names.push_back(preset.name);
            return names;
        });

    py::class_<PresetBundle, std::unique_ptr<PresetBundle, py::nodelete>>(host, "PresetBundle")
        .def_property_readonly("prints", [](PresetBundle& bundle) -> PresetCollection& {
            require_preset_access();
            return bundle.prints;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("printers", &printer_presets, py::return_value_policy::reference_internal)
        .def_property_readonly("filaments", [](PresetBundle& bundle) -> PresetCollection& {
            require_preset_access();
            return bundle.filaments;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("sla_prints", [](PresetBundle& bundle) -> PresetCollection& {
            require_preset_access();
            return bundle.sla_prints;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("sla_materials", [](PresetBundle& bundle) -> PresetCollection& {
            require_preset_access();
            return bundle.sla_materials;
        }, py::return_value_policy::reference_internal)
        .def("current_process_preset", [edited](py::object self) {
            require_preset_access();
            auto& bundle = self.cast<PresetBundle&>();
            return edited(collection_owner(self, bundle.prints));
        })
        .def("current_print_preset", [edited](py::object self) {
            require_preset_access();
            auto& bundle = self.cast<PresetBundle&>();
            return edited(collection_owner(self, bundle.prints));
        })
        .def("current_printer_preset", [edited](py::object self) {
            require_preset_access();
            auto& bundle = self.cast<PresetBundle&>();
            return edited(collection_owner(self, bundle.printers));
        })
        .def("current_filament_preset_names", [](PresetBundle& bundle) {
            require_preset_access();
            return bundle.filament_presets;
        })
        .def("current_filament_presets", [cache](py::object self) {
            require_preset_access();
            auto& bundle = self.cast<PresetBundle&>();
            auto owner = collection_owner(self, bundle.filaments);
            std::lock_guard<PresetCollection> lock(bundle.filaments);
            py::list result;
            for (const auto& name : bundle.filament_presets)
                result.append(preset_view(cache, owner, bundle.filaments, bundle.filaments.find_preset(name)));
            return result;
        })
        .def("full_config_keys", [](const PresetBundle& bundle) {
            return full_config(bundle).keys();
        })
        .def("full_config_value", [](const PresetBundle& bundle, const std::string& key) {
            return config_value_or_none(full_config(bundle), key);
        });
}

} // namespace Slic3r
