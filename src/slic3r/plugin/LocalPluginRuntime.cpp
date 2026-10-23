#include "LocalPluginRuntime.hpp"
#include "PluginDescriptor.hpp"
#include "PyPluginPackage.hpp"
#include "PythonPluginInterface.hpp"
#include "pluginTypes/script/ScriptPluginCapability.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace py = pybind11;

namespace Slic3r {
namespace {

struct Capture
{
    py::object              package_class;
    std::vector<py::object> capability_classes;
};

thread_local Capture*       active_capture = nullptr;
LocalPluginRuntime::AppInfo host_app_info;

class CaptureScope
{
public:
    explicit CaptureScope(Capture& capture) : m_previous(active_capture) { active_capture = &capture; }
    ~CaptureScope() { active_capture = m_previous; }
    CaptureScope(const CaptureScope&)            = delete;
    CaptureScope& operator=(const CaptureScope&) = delete;

private:
    Capture* m_previous;
};

class ScriptTrampoline : public ScriptPluginCapability
{
public:
    using ScriptPluginCapability::ScriptPluginCapability;
    std::string     get_name() const override { PYBIND11_OVERRIDE_PURE(std::string, ScriptPluginCapability, get_name); }
    void            on_load() override { PYBIND11_OVERRIDE(void, ScriptPluginCapability, on_load); }
    void            on_unload() override { PYBIND11_OVERRIDE(void, ScriptPluginCapability, on_unload); }
    ExecutionResult execute() override { PYBIND11_OVERRIDE_PURE(ExecutionResult, ScriptPluginCapability, execute); }
};

void require_capture()
{
    if (active_capture == nullptr)
        throw py::value_error("Plugin registration called outside plugin discovery context");
}

std::string read_metadata(const std::filesystem::path& file)
{
    std::ifstream input(file);
    if (!input)
        throw std::runtime_error("Cannot read plugin file");
    bool               in_metadata = false;
    std::ostringstream metadata;
    std::string        line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!in_metadata) {
            if (line == "# /// script")
                in_metadata = true;
            continue;
        }
        if (line == "# ///")
            return metadata.str();
        if (line.empty() || line.front() != '#')
            throw std::runtime_error("Malformed PEP 723 metadata block");
        metadata << line.substr(line.size() > 1 && line[1] == ' ' ? 2 : 1) << '\n';
    }
    throw std::runtime_error("Missing or unterminated PEP 723 script metadata");
}

void bind_local_script_api(py::module_& module)
{
    py::enum_<PluginCapabilityType>(module, "PluginType")
        .value("PrinterConnection", PluginCapabilityType::PrinterConnection)
        .value("Pages", PluginCapabilityType::Pages)
        .value("Analysis", PluginCapabilityType::Analysis)
        .value("Importer", PluginCapabilityType::Importer)
        .value("Exporter", PluginCapabilityType::Exporter)
        .value("Visualization", PluginCapabilityType::Visualization)
        .value("Script", PluginCapabilityType::Script)
        .value("SlicingPipeline", PluginCapabilityType::SlicingPipeline)
        .value("ResourceTexture", PluginCapabilityType::ResourceTexture)
        .value("Unknown", PluginCapabilityType::Unknown);
    py::enum_<PluginResult>(module, "PluginResult")
        .value("Success", PluginResult::Success)
        .value("Skipped", PluginResult::Skipped)
        .value("RecoverableError", PluginResult::RecoverableError)
        .value("FatalError", PluginResult::FatalError);
    py::class_<ExecutionResult>(module, "ExecutionResult")
        .def(py::init<>())
        .def(py::init<PluginResult, std::string, std::string>())
        .def_readwrite("status", &ExecutionResult::status)
        .def_readwrite("message", &ExecutionResult::message)
        .def_readwrite("data", &ExecutionResult::data)
        .def_static("success", &ExecutionResult::success, py::arg("message") = "", py::arg("data") = "")
        .def_static("skipped", &ExecutionResult::skipped, py::arg("message") = "")
        .def_static("failure", &ExecutionResult::failure, py::arg("status"), py::arg("message"), py::arg("data") = "");
    py::class_<PluginCapabilityInterface, std::shared_ptr<PluginCapabilityInterface>>(module, "PythonPluginBase")
        .def("get_name", &PluginCapabilityInterface::get_name)
        .def("get_type", &PluginCapabilityInterface::get_type)
        .def("on_load", &PluginCapabilityInterface::on_load)
        .def("on_unload", &PluginCapabilityInterface::on_unload);
    py::class_<PyPluginPackage, PyPluginPackageTrampoline>(module, "base")
        .def(py::init<>())
        .def("register_capabilities", &PyPluginPackage::register_capabilities);
    auto script = module.def_submodule("script");
    py::class_<ScriptPluginCapability, PluginCapabilityInterface, ScriptTrampoline,
               std::shared_ptr<ScriptPluginCapability>>(script, "ScriptPluginCapabilityBase")
        .def(py::init<>())
        .def("get_type", &ScriptPluginCapability::get_type)
        .def("execute", &ScriptPluginCapability::execute);
    module.def("plugin", [](py::object cls) {
        require_capture();
        if (!PyType_Check(cls.ptr()) || PyObject_IsSubclass(cls.ptr(), py::type::of<PyPluginPackage>().ptr()) != 1)
            throw py::value_error("@orca.plugin must decorate a subclass of orca.base");
        if (active_capture->package_class)
            throw py::value_error("Exactly one @orca.plugin class is allowed per plugin");
        active_capture->package_class = cls;
        return cls;
    });
    module.def("register_capability", [](py::object cls) {
        require_capture();
        if (!PyType_Check(cls.ptr()) || PyObject_IsSubclass(cls.ptr(), py::type::of<PluginCapabilityInterface>().ptr()) != 1)
            throw py::value_error("Registered class must inherit from a PluginCapability base");
        active_capture->capability_classes.push_back(std::move(cls));
    });
    auto host = module.def_submodule("host");
    host.def("app_info", []() {
        if (!host_app_info)
            throw std::runtime_error("Host application is unavailable");
        return py::module_::import("json").attr("loads")(host_app_info().dump());
    });
}

} // namespace

struct LocalPluginRuntime::Impl
{
    struct Package
    {
        py::object                                           module;
        py::object                                           instance;
        std::vector<py::object>                              owners;
        std::vector<std::shared_ptr<ScriptPluginCapability>> capabilities;
        std::vector<Script>                                  info;
        std::string                                          module_name;
    };

    bool                           initialized = false;
    std::thread::id                owner_thread;
    PyThreadState*                 main_thread_state = nullptr;
    std::map<std::string, Package> packages;
    std::uint64_t                  module_serial = 0;

    void require_owner() const
    {
        if (owner_thread != std::this_thread::get_id())
            throw std::runtime_error("Local Script operations must run on the initializing/UI thread");
    }
};

LocalPluginRuntime::LocalPluginRuntime() : m_impl(std::make_unique<Impl>()) {}
LocalPluginRuntime::~LocalPluginRuntime() { shutdown(); }

bool LocalPluginRuntime::initialized() const { return m_impl->initialized; }

bool LocalPluginRuntime::initialize(const std::filesystem::path& python_home, AppInfo app_info, std::string& error)
{
    if (m_impl->initialized) {
        m_impl->require_owner();
        return true;
    }
    if (Py_IsInitialized()) {
        error = "A Python interpreter is already active; this runtime cannot take ownership";
        return false;
    }
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.site_import = 0;
    config.parse_argv  = 0;
#ifdef _WIN32
    const PyStatus status = PyConfig_SetString(&config, &config.home, python_home.wstring().c_str());
#else
    const PyStatus status = PyConfig_SetBytesString(&config, &config.home, python_home.u8string().c_str());
#endif
    if (PyStatus_Exception(status)) {
        error = status.err_msg ? status.err_msg : "Failed to configure Python home";
        PyConfig_Clear(&config);
        return false;
    }
    try {
        py::initialize_interpreter(&config, 0, nullptr, false);
        PyConfig_Clear(&config);
        host_app_info             = std::move(app_info);
        m_impl->owner_thread      = std::this_thread::get_id();
        m_impl->initialized       = true;
        m_impl->main_thread_state = PyEval_SaveThread();
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        PyConfig_Clear(&config);
        return false;
    }
}

bool LocalPluginRuntime::inspect(const std::filesystem::path& file, PluginDescriptor& descriptor, std::string& error)
{
    descriptor = {};
    if (!initialized()) {
        error = "Python runtime is not initialized";
        return false;
    }
    m_impl->require_owner();
    py::gil_scoped_acquire gil;
    try {
        if (file.extension() != ".py")
            throw std::runtime_error("Local development currently supports .py packages only");
        const auto path = std::filesystem::canonical(file);
        if (!std::filesystem::is_regular_file(path))
            throw std::runtime_error("Plugin package is not a regular file");
        const auto metadata = py::module_::import("tomllib").attr("loads")(read_metadata(path)).cast<py::dict>();
        if (metadata.contains("dependencies")) {
            if (!py::isinstance<py::list>(metadata["dependencies"]))
                throw std::runtime_error("Plugin dependencies must be an array of strings");
            descriptor.dependencies = metadata["dependencies"].cast<std::vector<std::string>>();
        }
        const auto identity = metadata["tool"]["orcaslicer"]["plugin"].cast<py::dict>();
        if (!identity.contains("name") || identity["name"].cast<std::string>().empty())
            throw std::runtime_error("Plugin metadata must declare a nonempty name");
        descriptor.name           = sanitize_plugin_name(identity["name"].cast<std::string>());
        descriptor.version        = identity.contains("version") ? identity["version"].cast<std::string>() : "";
        descriptor.description    = identity.contains("description") ? identity["description"].cast<std::string>() : "";
        descriptor.author         = identity.contains("author") ? identity["author"].cast<std::string>() : "";
        descriptor.plugin_key     = make_local_plugin_key(path.stem().u8string());
        descriptor.entry_path     = path.u8string();
        descriptor.plugin_root    = path.parent_path().u8string();
        descriptor.metadata_valid = true;
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error            = ex.what();
        descriptor.error = error;
        return false;
    }
}

bool LocalPluginRuntime::load(const std::filesystem::path& file, std::string& error)
{
    if (!initialized()) {
        error = "Python runtime is not initialized";
        return false;
    }
    m_impl->require_owner();
    py::gil_scoped_acquire gil;
    std::string            module_name;
    try {
        PluginDescriptor descriptor;
        if (!inspect(file, descriptor, error))
            throw std::runtime_error(error);
        const auto        path = std::filesystem::u8path(descriptor.entry_path);
        const std::string key  = descriptor.plugin_key;
        if (m_impl->packages.count(key) != 0)
            throw std::runtime_error("Package already loaded; restart the development host to replace it");
        if (!descriptor.dependencies.empty())
            throw std::runtime_error("Dependency installation belongs to S03 and is not enabled in this spike");
        const std::string version = descriptor.version;

        module_name                  = "_snapmaker_orca_local_" + std::to_string(++m_impl->module_serial);
        auto util                    = py::module_::import("importlib.util");
        auto spec                    = util.attr("spec_from_file_location")(module_name, path.u8string());
        auto module                  = util.attr("module_from_spec")(spec);
        auto modules                 = py::module_::import("sys").attr("modules");
        modules[module_name.c_str()] = module;
        Capture      capture;
        CaptureScope scope(capture);
        spec.attr("loader").attr("exec_module")(module);
        if (!capture.package_class)
            throw std::runtime_error("Missing @orca.plugin package class");
        Impl::Package package;
        package.module      = std::move(module);
        package.module_name = module_name;
        package.instance    = capture.package_class();
        package.instance.attr("register_capabilities")();
        if (capture.capability_classes.empty())
            throw std::runtime_error("Package registered no capabilities");
        for (const auto& cls : capture.capability_classes) {
            auto              owner      = cls();
            auto              capability = owner.cast<std::shared_ptr<ScriptPluginCapability>>();
            const std::string name       = capability->get_name();
            if (name.empty() || name.find(';') != std::string::npos)
                throw std::runtime_error("Capability name must be nonempty and cannot contain ';'");
            for (const auto& existing : scripts())
                if (existing.name == name)
                    throw std::runtime_error("Duplicate Script capability name: " + name);
            for (const auto& existing : package.info)
                if (existing.name == name)
                    throw std::runtime_error("Duplicate Script capability name: " + name);
            capability->set_audit_plugin_key(key);
            capability->set_resolved_identity(name, PluginCapabilityType::Script);
            package.info.push_back({key, name, version});
            package.owners.push_back(std::move(owner));
            package.capabilities.push_back(std::move(capability));
        }
        std::size_t loaded = 0;
        try {
            for (const auto& capability : package.capabilities) {
                capability->on_load();
                ++loaded;
            }
        } catch (...) {
            while (loaded > 0) {
                try {
                    package.capabilities[--loaded]->on_unload();
                } catch (...) {}
            }
            throw;
        }
        m_impl->packages.emplace(key, std::move(package));
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        if (!module_name.empty())
            py::module_::import("sys").attr("modules").attr("pop")(module_name, py::none());
        return false;
    }
}

std::vector<LocalPluginRuntime::Script> LocalPluginRuntime::scripts() const
{
    m_impl->require_owner();
    std::vector<Script> result;
    for (const auto& entry : m_impl->packages)
        result.insert(result.end(), entry.second.info.begin(), entry.second.info.end());
    return result;
}

bool LocalPluginRuntime::is_loaded(const std::string& package_key) const
{
    if (!initialized())
        return false;
    m_impl->require_owner();
    return m_impl->packages.count(package_key) != 0;
}

bool LocalPluginRuntime::unload(const std::string& package_key, std::string& error)
{
    error.clear();
    if (!initialized())
        return true;
    m_impl->require_owner();
    py::gil_scoped_acquire gil;
    auto                   found = m_impl->packages.find(package_key);
    if (found == m_impl->packages.end())
        return true;
    for (auto it = found->second.capabilities.rbegin(); it != found->second.capabilities.rend(); ++it) {
        try {
            (*it)->on_unload();
        } catch (const std::exception& ex) {
            if (!error.empty())
                error += "\n";
            error += ex.what();
        }
    }
    py::module_::import("sys").attr("modules").attr("pop")(found->second.module_name, py::none());
    m_impl->packages.erase(found);
    return error.empty();
}

LocalPluginRuntime::Result LocalPluginRuntime::execute(const std::string& key, const std::string& name)
{
    if (!initialized())
        return {false, "Python runtime is not initialized", {}};
    m_impl->require_owner();
    py::gil_scoped_acquire gil;
    const auto             found = m_impl->packages.find(key);
    if (found == m_impl->packages.end())
        return {false, "Package is not loaded", {}};
    const auto& package = found->second;
    for (std::size_t i = 0; i < package.info.size(); ++i) {
        if (package.info[i].name != name)
            continue;
        try {
            const ExecutionResult result = package.capabilities[i]->execute();
            return {result.status == PluginResult::Success || result.status == PluginResult::Skipped, result.message, result.data};
        } catch (const std::exception& ex) {
            return {false, ex.what(), {}};
        }
    }
    return {false, "Script capability is not loaded", {}};
}

void LocalPluginRuntime::shutdown()
{
    if (!initialized())
        return;
    m_impl->require_owner();
    PyEval_RestoreThread(m_impl->main_thread_state);
    m_impl->main_thread_state = nullptr;
    for (auto& entry : m_impl->packages) {
        for (auto it = entry.second.capabilities.rbegin(); it != entry.second.capabilities.rend(); ++it) {
            try {
                (*it)->on_unload();
            } catch (...) {}
        }
        py::module_::import("sys").attr("modules").attr("pop")(entry.second.module_name, py::none());
    }
    m_impl->packages.clear();
    host_app_info = {};
    py::finalize_interpreter();
    m_impl->initialized = false;
}

} // namespace Slic3r

PYBIND11_EMBEDDED_MODULE(orca, module) { Slic3r::bind_local_script_api(module); }
