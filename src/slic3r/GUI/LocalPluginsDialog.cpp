#include "LocalPluginsDialog.hpp"
#include "slic3r/plugin/host/PluginAppInfoIdentity.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "common_func/common_func.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/plugin/LocalPluginRuntime.hpp"
#include "slic3r/plugin/LocalPluginStore.hpp"
#ifdef ORCA_PYTHON_PLUGINS
#include "slic3r/plugin/PluginDependencyInstaller.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/host/PluginPagesLocal.hpp"
#include "slic3r/plugin/host/PluginHostUi.hpp"
#include "slic3r/plugin/host/PluginConfigLocal.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include <thread>
#include <atomic>
#include "Notebook.hpp"
#include "Tab.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include <future>
#include <set>
#include "Plater.hpp"
#include "PluginSlicingGui.hpp"
#include "PluginWorkflowSmoke.hpp"
#include <wx/dirdlg.h>
#include <wx/textdlg.h>
#include <wx/evtloop.h>
#include <wx/wrapsizer.h>
#include <chrono>
#endif

#include <boost/log/trivial.hpp>
#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>

namespace Slic3r::GUI {
#ifdef ORCA_PYTHON_PLUGINS
// Isolated acceptance fixture; compiled only into the plugins-enabled GUI target.
nlohmann::json run_plugin_legacy_states_smoke(const std::filesystem::path& fixture, const std::filesystem::path& report);
nlohmann::json run_plugin_dualhost_gui(const std::filesystem::path& request, const std::filesystem::path& report);
nlohmann::json run_plugin_texture_mix_smoke(const std::filesystem::path& report);
void test_plugin_dock_reentrant_teardown(Plater* plater);
nlohmann::json test_plugin_dock_layout_restore(Plater* plater);
nlohmann::json test_plugin_dock_restart(const std::string&, const std::filesystem::path&, bool);
nlohmann::json test_plugin_pages_restart(const std::string& plugin_key, const std::filesystem::path& state_file, bool restarted);
#endif
namespace {

LocalPluginRuntime& local_runtime()
{
    static LocalPluginRuntime runtime;
    return runtime;
}

std::map<std::string, std::string> load_errors;

class LocalPluginStartupTimer final : public wxTimer
{
public:
    LocalPluginStartupTimer(std::function<bool()> ready, std::function<void()> run) : m_ready(std::move(ready)), m_run(std::move(run)) {}

    void Notify() override
    {
        if (!m_ready())
            return;
        for (auto it = wxTopLevelWindows.begin(); it != wxTopLevelWindows.end(); ++it) {
            const auto* dialog = dynamic_cast<wxDialog*>(*it);
            if (dialog && dialog->IsModal())
                return;
        }
        Stop();
        m_run();
    }

private:
    std::function<bool()> m_ready;
    std::function<void()> m_run;
};

std::unique_ptr<LocalPluginStartupTimer> startup_timer;

std::filesystem::path python_home()
{
#ifdef __APPLE__
    const auto executable = std::filesystem::u8path(wxStandardPaths::Get().GetExecutablePath().ToUTF8().data());
    const auto bundled    = executable.parent_path().parent_path() / "Frameworks/python";
    if (std::filesystem::is_regular_file(bundled / "lib/python3.12/encodings/__init__.py"))
        return bundled;
#endif
    // Explicit development prefix only; never fall back to a system Python.
    return std::filesystem::u8path(ORCA_PLUGIN_PYTHON_HOME);
}

nlohmann::json app_info()
{
    if (!wxIsMainThread())
        throw std::runtime_error("app_info() must be called from the UI thread");
    auto& app = wxGetApp();
    nlohmann::json info = {{"version", Snapmaker_VERSION},
            {"build", GIT_COMMIT_HASH},
            {"mode", wxGetApp().is_editor() ? "editor" : "gcode viewer"},
            {"language", std::string(wxGetApp().current_language_code_safe().ToUTF8())},
            {"app_config_version", app.app_config && app.app_config->orig_version().valid() ? app.app_config->orig_version().to_string_sf() : std::string()},
            {"stealth_mode", app.app_config && app.app_config->get_stealth_mode()}};
    append_plugin_app_info_identity(info, app.gateway_account().is_login, app.is_user_login(), NetworkAgent::network_module_snapshot());
    return info;
}

bool page_host_initialized = false;
bool initialize_runtime(std::string& error)
{
    if (!local_runtime().initialize(python_home(), app_info, error))
        return false;
#ifdef ORCA_PYTHON_PLUGINS
    initialize_plugin_slicing_gui(wxGetApp().plater());
    if (!page_host_initialized && wxGetApp().plater()) {
        initialize_local_plugin_pages(dynamic_cast<Notebook*>(wxGetApp().plater()->GetParent()));
        page_host_initialized = true;
    }
#endif
    return true;
}

LocalPluginStore local_store()
{
    return LocalPluginStore(std::filesystem::u8path(data_dir()) / "orca_plugins", [](const auto& file, auto& descriptor, auto& error) {
        return local_runtime().inspect(file, descriptor, error);
    });
}

bool load_installed(const PluginDescriptor& descriptor, std::string& error)
{
    if (!descriptor.enabled || descriptor.is_invalid_package() || !descriptor.install_state_valid) {
        error = descriptor.has_error() ? descriptor.error : "Plugin is disabled or has no valid install state";
        return false;
    }
    auto& runtime = local_runtime();
    if (runtime.is_loaded(descriptor.plugin_key))
        return true;
    if (!runtime.load(std::filesystem::u8path(descriptor.entry_path), error)) {
        load_errors[descriptor.plugin_key] = error;
        return false;
    }
    load_errors.erase(descriptor.plugin_key);
    return true;
}

class LocalPluginsDialog : public wxDialog
{
public:
    explicit LocalPluginsDialog(wxWindow* parent)
        : wxDialog(parent,
                   wxID_ANY,
                   _L("Local Plugins — development"),
                   wxDefaultPosition,
                   wxSize(740, 460),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        auto* layout = new wxBoxSizer(wxVERTICAL);
#ifdef ORCA_PYTHON_PLUGINS
        auto* notice =
            new wxStaticText(this, wxID_ANY,
                             _L("Local Python plugins: packages execute code on this computer.\n"
                                "Upstream runtime and permission audit enabled. Development build; SDK migration is in progress."));
#else
        auto* notice = new wxStaticText(this, wxID_ANY,
                                        _L("Development build: trusted, dependency-free Python Script plugins only.\n"
                                           "Installed plugins run automatically at startup. Permission audit is not enabled yet."));
#endif
        layout->Add(notice, 0, wxALL | wxEXPAND, 12);
        m_list = new wxListBox(this, wxID_ANY);
        layout->Add(m_list, 1, wxLEFT | wxRIGHT | wxEXPAND, 12);
        auto*      buttons    = new wxWrapSizer(wxHORIZONTAL);
        const auto add_button = [&](const wxString& label, auto handler) {
            auto* button = new wxButton(this, wxID_ANY, label);
            buttons->Add(button, 0, wxRIGHT, 6);
            button->Bind(wxEVT_BUTTON, handler);
        };
        add_button(_L("Install Plugin"), [this](wxCommandEvent&) { install(); });
        add_button(_L("Run Script"), [this](wxCommandEvent&) { run(); });
        add_button(_L("Enable / Disable"), [this](wxCommandEvent&) { toggle(); });
        add_button(_L("Remove"), [this](wxCommandEvent&) { remove(); });
#ifdef ORCA_PYTHON_PLUGINS
        add_button(_L("Slicing plugins"), [this](wxCommandEvent&) { select_slicing_plugins(); });
        add_button(_L("Config"), [this](wxCommandEvent&) { configure(); });
        add_button(_L("Dependencies"), [this](wxCommandEvent&) { dependencies(); });
#endif
        add_button(_L("Refresh"), [this](wxCommandEvent&) { refresh(); });
        layout->Add(buttons, 0, wxALL, 12);
        layout->Add(CreateButtonSizer(wxCLOSE), 0, wxALL | wxALIGN_RIGHT, 12);
        SetSizer(layout);
        SetMinSize(wxSize(740, 380));
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
        refresh();
        CentreOnParent();
    }

private:
    wxListBox*                    m_list = nullptr;
    std::vector<PluginDescriptor> m_entries;

    void show_error(const std::string& error)
    {
        wxMessageBox(wxString::FromUTF8(error), _L("Local plugin operation failed"), wxOK | wxICON_ERROR, this);
    }

    const PluginDescriptor* selected()
    {
        const auto selection = m_list->GetSelection();
        if (selection == wxNOT_FOUND) {
            wxMessageBox(_L("Select a plugin first."), GetTitle(), wxOK | wxICON_INFORMATION, this);
            return nullptr;
        }
        return &m_entries.at(static_cast<std::size_t>(selection));
    }

    void refresh()
    {
        std::string error;
        m_entries = local_store().scan(error);
        m_list->Clear();
        for (const auto& entry : m_entries) {
            std::string status = entry.enabled ? "enabled" : "disabled";
            if (entry.has_error())
                status = entry.error;
            else if (load_errors.count(entry.plugin_key))
                status = load_errors.at(entry.plugin_key);
            else if (local_runtime().is_loaded(entry.plugin_key))
                status += ", loaded";
            std::replace(status.begin(), status.end(), '\n', ' ');
            m_list->Append(wxString::FromUTF8(entry.plugin_key + " | " + entry.name + " " + entry.version + " | " + status));
        }
        if (!error.empty())
            show_error(error);
    }

#ifdef ORCA_PYTHON_PLUGINS
    void dependencies()
    {
        wxArrayString choices;
        choices.Add(_L("Allow dependency downloads"));
        choices.Add(_L("Offline wheelhouse"));
        wxSingleChoiceDialog picker(this, _L("Dependency installation mode"), GetTitle(), choices);
        if (picker.ShowModal() != wxID_OK)
            return;
        plugin_loader::DependencyInstallOptions options;
        options.offline = picker.GetSelection() == 1;
        if (options.offline) {
            wxDirDialog directory(this, _L("Select a wheelhouse for this Python version and platform"));
            if (directory.ShowModal() != wxID_OK)
                return;
            options.wheelhouse = directory.GetPath().utf8_string();
        }
        std::string error;
        if (!plugin_loader::configure_dependency_install(options, error))
            show_error(error);
    }
    void configure()
    {
        const auto* entry = selected();
        if (!entry)
            return;
        auto&         manager = PluginManager::instance();
        const auto    caps    = manager.get_plugin_capabilities(entry->plugin_key, PluginCapabilityType::Unknown, false);
        wxArrayString choices;
        for (const auto& cap : caps)
            choices.Add(wxString::FromUTF8(cap->name()));
        if (choices.empty()) {
            show_error("Load the package before configuring its capabilities");
            return;
        }
        wxSingleChoiceDialog picker(this, _L("Select a capability"), GetTitle(), choices);
        if (picker.ShowModal() != wxID_OK)
            return;
        const auto        id     = caps.at(picker.GetSelection())->identity();
        try {
            show_local_plugin_config(this, id);
        } catch (const std::exception& ex) {
            show_error(ex.what());
        }
    }
#endif

    void install()
    {
        wxFileDialog picker(this, _L("Install a trusted local plugin"), wxEmptyString, wxEmptyString,
                            "Python plugin (*.py;*.whl)|*.py;*.whl", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (picker.ShowModal() != wxID_OK)
            return;
        if (wxMessageBox(_L("This plugin can access files and the network with your account's privileges. "
                            "Install and automatically run it on future starts only if you trust its source."),
                         GetTitle(), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES)
            return;
        PluginDescriptor descriptor;
        std::string      error;
        if (!local_store().install(std::filesystem::u8path(picker.GetPath().ToUTF8().data()), descriptor, error))
            show_error(error);
        else if (!load_installed(descriptor, error))
            show_error("Plugin activation failed: " + error);
        refresh();
    }

    void run()
    {
        const auto* entry = selected();
        if (!entry)
            return;
        std::string error;
        if (!load_installed(*entry, error)) {
            show_error(error);
            refresh();
            return;
        }
        std::vector<LocalPluginRuntime::Script> scripts;
        wxArrayString                           choices;
        for (const auto& script : local_runtime().scripts())
            if (script.package_key == entry->plugin_key) {
                scripts.push_back(script);
                choices.Add(wxString::FromUTF8(script.name));
            }
        wxSingleChoiceDialog picker(this, _L("Select a Script capability"), GetTitle(), choices);
        if (picker.ShowModal() != wxID_OK)
            return;
        const auto& script = scripts.at(static_cast<std::size_t>(picker.GetSelection()));
        const auto  result = local_runtime().execute(script.package_key, script.name);
        wxMessageBox(wxString::FromUTF8(result.message), result.ok ? _L("Plugin result") : _L("Plugin failed"),
                     wxOK | (result.ok ? wxICON_INFORMATION : wxICON_ERROR), this);
    }

#ifdef ORCA_PYTHON_PLUGINS
    void select_slicing_plugins()
    {
        auto* bundle = wxGetApp().preset_bundle;
        auto* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (!bundle || !tab) return;
        auto config = bundle->prints.get_edited_preset().config;
        const auto* old = config.option<ConfigOptionStrings>("slicing_pipeline_plugin");
        const auto* manifest = config.option<ConfigOptionStrings>("plugins");
        const std::vector<std::string> old_names = old ? old->values : std::vector<std::string>();
        struct Choice { std::string name, ref; };
        std::vector<Choice> choices;
        wxArrayString labels;
        wxArrayInt selected;
        // Preserve unresolved references visibly. Selecting none is an explicit way
        // to recover a project whose required package is unavailable.
        for (const auto& name : old_names) {
            std::string ref;
            bool ambiguous = false;
            if (manifest) for (const auto& value : manifest->values) {
                auto parsed = parse_capability_ref(value);
                if (parsed && parsed->capability_name == name) {
                    if (!ref.empty()) ambiguous = true;
                    ref = value;
                }
            }
            if (ambiguous) ref.clear();
            selected.Add(int(choices.size()));
            choices.push_back({name, ref});
            labels.Add(wxString::FromUTF8(name + " [preset: " + ref + "]"));
        }
        for (const auto& cap : PluginManager::instance().get_plugin_capabilities("", PluginCapabilityType::SlicingPipeline)) {
            const auto ref = cap->audit_plugin_key() + ";;" + cap->name();
            if (std::any_of(choices.begin(), choices.end(), [&](const auto& item) { return item.ref == ref; })) continue;
            choices.push_back({cap->name(), ref});
            labels.Add(wxString::FromUTF8(cap->name() + " [" + cap->audit_plugin_key() + "]"));
        }
        wxMultiChoiceDialog picker(this, _L("Select slicing plugins for the current process preset. Clear all to use native slicing."), GetTitle(), labels);
        picker.SetSelections(selected);
        if (picker.ShowModal() != wxID_OK) return;
        std::vector<std::string> names, refs;
        std::vector<Choice> ordered;
        if (manifest) for (const auto& value : manifest->values) {
            auto parsed = parse_capability_ref(value);
            if (!parsed || std::find(old_names.begin(), old_names.end(), parsed->capability_name) == old_names.end()) refs.push_back(value);
        }
        std::set<std::string> unique_names;
        for (int index : picker.GetSelections()) {
            const auto& choice = choices.at(size_t(index));
            if (choice.ref.empty() || !unique_names.insert(choice.name).second) {
                show_error("Select one valid package per capability name: " + choice.name); return;
            }
            ordered.push_back(choice);
        }
        CapabilityConfigDocument overrides;
        const auto* raw = config.option<ConfigOptionString>("print_plugin_config_overrides");
        std::string error;
        if (!parse_plugin_overrides(raw ? raw->value : std::string(), overrides, error)) {
            show_error(error); return;
        }
        if (!ordered.empty()) {
            wxDialog order(this, wxID_ANY, _L("Slicing plugin order and configuration"), wxDefaultPosition,
                           wxSize(680, 430), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
            auto* layout = new wxBoxSizer(wxVERTICAL);
            auto* instruction = new wxStaticText(&order, wxID_ANY,
                _L("Plugins run from top to bottom. Configuration changes apply to this process preset when you press OK."));
            instruction->Wrap(620);
            layout->Add(instruction, 0, wxALL | wxEXPAND, 12);
            auto* list = new wxListBox(&order, wxID_ANY);
            layout->Add(list, 1, wxLEFT | wxRIGHT | wxEXPAND, 12);
            auto* controls = new wxBoxSizer(wxHORIZONTAL);
            auto* up = new wxButton(&order, wxID_ANY, _L("Move up"));
            auto* down = new wxButton(&order, wxID_ANY, _L("Move down"));
            auto* configure = new wxButton(&order, wxID_ANY, _L("Configure for this preset"));
            controls->Add(up, 0, wxRIGHT, 8);
            controls->Add(down, 0, wxRIGHT, 8);
            controls->Add(configure, 0);
            layout->Add(controls, 0, wxALL, 12);
            layout->Add(order.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxEXPAND, 12);
            order.SetSizer(layout);
            const auto identity = [](const Choice& choice) {
                const auto reference = parse_capability_ref(choice.ref);
                if (!reference) throw std::runtime_error("Invalid slicing capability reference");
                return PluginCapabilityId{PluginCapabilityType::SlicingPipeline, choice.name,
                                          reference->uuid.empty() ? reference->name : reference->uuid};
            };
            const auto update_buttons = [&] {
                const int index = list->GetSelection();
                up->Enable(index > 0);
                down->Enable(index >= 0 && size_t(index + 1) < ordered.size());
                configure->Enable(index >= 0 && bool(PluginManager::instance().get_plugin_capability(identity(ordered[size_t(index)]), false)));
            };
            const auto refresh = [&](int index) {
                list->Clear();
                for (const auto& choice : ordered)
                    list->Append(wxString::FromUTF8(choice.name) + (overrides.contains(identity(choice)) ? " *" : ""));
                list->SetSelection(index);
                update_buttons();
            };
            list->Bind(wxEVT_LISTBOX, [&](wxCommandEvent&) { update_buttons(); });
            up->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
                const int index = list->GetSelection();
                if (index > 0) { std::swap(ordered[size_t(index)], ordered[size_t(index - 1)]); refresh(index - 1); }
            });
            down->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
                const int index = list->GetSelection();
                if (index >= 0 && size_t(index + 1) < ordered.size()) {
                    std::swap(ordered[size_t(index)], ordered[size_t(index + 1)]); refresh(index + 1);
                }
            });
            configure->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
                const int index = list->GetSelection();
                if (index < 0) return;
                try { show_preset_plugin_config(&order, identity(ordered[size_t(index)]), overrides); }
                catch (const std::exception& ex) { wxMessageBox(wxString::FromUTF8(ex.what()), _L("Plugin configuration"), wxOK | wxICON_ERROR, &order); }
                refresh(index);
            });
            refresh(0);
            if (order.ShowModal() != wxID_OK) return;
        }
        for (const auto& choice : ordered) { names.push_back(choice.name); refs.push_back(choice.ref); }
        config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings(names));
        config.set_key_value("plugins", new ConfigOptionStrings(refs));
        config.set_key_value("print_plugin_config_overrides", new ConfigOptionString(serialize_plugin_overrides(overrides)));
        prune_stale_plugin_overrides(config, "print_plugin_config_overrides");
        tab->load_config(config);
        if (wxGetApp().plater() && !bundle->filament_presets.empty()) wxGetApp().plater()->on_config_change(bundle->full_config());
    }
#endif

    void toggle()
    {
        const auto* entry = selected();
        if (!entry)
            return;
        if (entry->has_error()) {
            show_error(entry->error);
            return;
        }
        const bool  enable = !entry->enabled;
        std::string error;
        if (!enable && !local_runtime().unload(entry->plugin_key, error)) {
            show_error(error);
            return;
        }
        if (!local_store().set_enabled(entry->plugin_key, enable, error)) {
            show_error(error);
            return;
        }
        if (!enable) {
            load_errors.erase(entry->plugin_key);
        } else {
            auto enabled    = *entry;
            enabled.enabled = true;
            if (!load_installed(enabled, error))
                show_error(error);
        }
        refresh();
    }

    void remove()
    {
        const auto* entry = selected();
        if (!entry)
            return;
        if (wxMessageBox(_L("Remove the selected plugin? Plugin data will be preserved."), GetTitle(),
                         wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
            return;
        std::string error;
        if (!local_runtime().unload(entry->plugin_key, error)) {
            show_error(error);
            return;
        }
        if (!local_store().remove(entry->plugin_key, error))
            show_error(error);
        else
            load_errors.erase(entry->plugin_key);
        refresh();
    }
};

} // namespace

void start_local_plugin_spike()
{
    std::string error;
    if (!initialize_runtime(error)) {
        BOOST_LOG_TRIVIAL(error) << "Local plugin initialization: " << error;
        return;
    }
    const auto entries = local_store().scan(error);
    if (!error.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Local plugin scan: " << error;
        return;
    }
    for (const auto& entry : entries)
        if (entry.enabled && !entry.has_error() && !load_installed(entry, error))
            BOOST_LOG_TRIVIAL(error) << "Local plugin '" << entry.plugin_key << "': " << error;
}

void show_local_plugin_spike(wxWindow* parent)
{
    std::string error;
    if (!initialize_runtime(error)) {
        wxMessageBox(wxString::FromUTF8(error), _L("Python initialization failed"), wxOK | wxICON_ERROR, parent);
        return;
    }
    LocalPluginsDialog dialog(parent);
    dialog.ShowModal();
}

void shutdown_local_plugin_spike()
{
#ifdef ORCA_PYTHON_PLUGINS
    shutdown_plugin_slicing_gui();
    shutdown_local_plugin_pages();
#endif
    page_host_initialized = false;
    startup_timer.reset();
    local_runtime().shutdown();
    load_errors.clear();
}

void schedule_local_plugin_start(std::function<bool()> ready, std::function<void()> run)
{
    startup_timer = std::make_unique<LocalPluginStartupTimer>(std::move(ready), std::move(run));
    startup_timer->Start(100);
}

void record_local_plugin_smoke_exit(int result)
{
    wxString report;
    if (!wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_REPORT", &report) || report.empty())
        return;
    const auto report_path = std::filesystem::u8path(report.ToUTF8().data());
    // Normal-close smoke modes deliberately leave the interpreter and plugin UI
    // alive until MainFrame::Close and GUI_App::OnExit complete the real teardown.
    try {
        nlohmann::json evidence;
        std::ifstream input(report_path);
        if (input) {
            input >> evidence;
            input.close();
            if (evidence.value("normal_window_close", false) &&
                evidence.value("pid", long(-1)) == static_cast<long>(wxGetProcessId())) {
                evidence["interpreter_stopped"] = !local_runtime().initialized();
                evidence["normal_close_completed"] = true;
                std::ofstream updated(report_path);
                updated << evidence.dump(2) << '\n';
            }
        }
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "Could not finalize normal-close smoke evidence: " << ex.what();
    }
    std::ofstream output(std::filesystem::u8path(report.ToUTF8().data()).concat(".exit"));
    output << nlohmann::json{{"on_exit", true}, {"result", result}, {"pid", static_cast<long>(wxGetProcessId())}}.dump(2) << '\n';
}

void run_local_plugin_smoke(const std::filesystem::path& file, const std::filesystem::path& report)
{
    nlohmann::json evidence{{"ok", false}, {"ui_thread", wxIsMainThread()}, {"data_dir", data_dir()}};
    auto&          runtime = local_runtime();
    bool normal_window_close = false;
    try {
        evidence["pid"] = static_cast<long>(wxGetProcessId());
        wxString expected_data_dir;
        if (!wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_DATADIR", &expected_data_dir) || expected_data_dir.empty() ||
            std::filesystem::weakly_canonical(std::filesystem::u8path(expected_data_dir.ToUTF8().data())) !=
                std::filesystem::weakly_canonical(std::filesystem::u8path(data_dir())))
            throw std::runtime_error("GUI smoke refused to run outside its explicit isolated data directory");
        std::string error;
        evidence["python_home"] = python_home().u8string();
        if (!initialize_runtime(error))
            throw std::runtime_error(error);
        wxString mode;
        wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_MODE", &mode);
        if (mode.empty() || mode == "install" || mode == "pages" || mode == "pages_multi" || mode == "dock_write" || mode == "ui" || mode == "host" || mode == "config" || mode == "replacement" || mode == "slicing" || mode == "preset" || mode == "workflow" || mode == "texture") {
            PluginDescriptor installed;
            if (!local_store().install(file, installed, error) || !load_installed(installed, error))
                throw std::runtime_error(error);
            evidence["installed_entry"] = installed.entry_path;
        } else if (mode == "restart" || mode == "pages_multi_restart" || mode == "dock_restart" || mode == "disabled" || mode == "manager") {
            start_local_plugin_spike();
            if (mode == "manager")
                show_local_plugin_spike(wxGetApp().mainframe);
        } else if (mode == "legacy_states") {
            // Keep the first baseline truly uninstalled; the fixture owns transitions.
        } else if (mode == "dualhost") {
            // The canonical fixture owns install/enable/restart operations.
        } else {
            throw std::runtime_error("Unknown GUI plugin smoke mode");
        }
        evidence["mode"]         = std::string(mode.ToUTF8());
        evidence["host"]         = app_info();
        const auto scripts       = runtime.scripts();
        evidence["script_count"] = scripts.size();
#ifdef ORCA_PYTHON_PLUGINS
        if (mode == "legacy_states") {
            normal_window_close = true;
            evidence["normal_window_close"] = true;
            evidence.update(run_plugin_legacy_states_smoke(file, report));
        } else if (mode == "dualhost") {
            normal_window_close = true;
            evidence["normal_window_close"] = true;
            evidence["dualhost"] = run_plugin_dualhost_gui(file, report);
            evidence["ok"] = evidence["dualhost"].value("passed", false);
        } else if (mode == "dock_write" || mode == "dock_restart") {
            wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
            if (scripts.size() != 1) throw std::runtime_error("Dock restart needs one Script");
            const auto result = runtime.execute(scripts.front().package_key, scripts.front().name);
            if (!result.ok) throw std::runtime_error(result.message);
            evidence["dock_restart"] = test_plugin_dock_restart(scripts.front().package_key,
                std::filesystem::u8path(data_dir()) / "dock-restart-state.json", mode == "dock_restart");
            evidence["ok"] = true;
            evidence["normal_window_close"] = normal_window_close = true;
        } else if (mode == "pages_multi" || mode == "pages_multi_restart") {
            std::string plugin_key;
            {
                const auto capabilities = PluginManager::instance().get_plugin_capabilities("", PluginCapabilityType::Pages);
                if (capabilities.size() != 2 || capabilities.front()->identity().plugin_key != capabilities.back()->identity().plugin_key)
                    throw std::runtime_error("Multi-page smoke needs one package with exactly two Pages");
                plugin_key = capabilities.front()->identity().plugin_key;
            }
            evidence["multi_pages"] = test_plugin_pages_restart(plugin_key,
                std::filesystem::u8path(data_dir()) / "pages-multi-state.json", mode == "pages_multi_restart");
            evidence["ok"] = true;
        } else if (mode == "texture") {
            wxString mix;
            if (wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_TEXTURE_MIX", &mix) && mix == "1")
                evidence.update(run_plugin_texture_mix_smoke(report));
            else
                evidence.update(run_plugin_texture_smoke(report));
        } else if (mode == "workflow") {
            test_plugin_dock_reentrant_teardown(wxGetApp().plater());
            evidence["dock_layout_restore"] = test_plugin_dock_layout_restore(wxGetApp().plater());
            evidence["dock_reentrant_teardown"] = true;
            evidence.update(run_plugin_workflow_smoke(report));
        } else if (mode == "slicing" || mode == "preset") {
            auto& manager = PluginManager::instance();
            const auto capabilities = manager.get_plugin_capabilities("", PluginCapabilityType::SlicingPipeline);
            if (capabilities.size() != 1) throw std::runtime_error("Slicing smoke needs one capability");
            const auto id = capabilities.front()->identity();
            if (!manager.get_config().store_capability_config(id, {{"factor", .5}})) throw std::runtime_error("Could not save slice config");
            // Exercise the actual developer selector, then consume its saved
            // options on a worker. This is not the Plater export/upload test.
            show_local_plugin_spike(wxGetApp().mainframe);
            const auto& preset = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            const auto* names = preset.option<ConfigOptionStrings>("slicing_pipeline_plugin");
            if (!names || names->values != std::vector<std::string>{id.name}) throw std::runtime_error("Select the test slicing capability before closing the manager");
            evidence["slicing_selection"] = names->values;
            const double expected_width = mode == "preset" ? 7.5 : 5.;
            if (mode == "preset") {
                CapabilityConfigDocument overrides;
                const auto* raw = preset.option<ConfigOptionString>("print_plugin_config_overrides");
                if (!raw || !parse_plugin_overrides(raw->value, overrides, error)) throw std::runtime_error("Preset override was not saved");
                const auto value = overrides.find(id);
                if (!value || value->config.value("factor", 0.) != .75)
                    throw std::runtime_error("Set this preset's factor to 0.75 using Configure for this preset");
                const auto global = manager.get_config().get_config(id);
                if (!global || global->config.value("factor", 0.) != .5)
                    throw std::runtime_error("Preset configuration changed global configuration");
                evidence["preset_override_saved"] = true;
                evidence["preset_global_config_unchanged"] = true;
            }
            Model model;
            auto* object = model.add_object();
            object->add_volume(TriangleMesh(make_cube(10., 20., 1.)));
            object->add_instance(); object->ensure_on_bed();
            DynamicPrintConfig config;
            const auto& defaults = FullPrintConfig::defaults();
            config.apply(static_cast<const PrintConfig&>(defaults), true);
            config.apply(static_cast<const PrintObjectConfig&>(defaults), true);
            config.apply(static_cast<const PrintRegionConfig&>(defaults), true);
            for (const auto& key : {"slicing_pipeline_plugin", "plugins", "print_plugin_config_overrides"})
                if (const auto* option = preset.option(key)) config.set_key_value(key, option->clone());
            Print print;
            print.apply(model, config); print.set_status_silent();
            auto worker = std::async(std::launch::async, [&] { print.process(); });
            while (worker.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
                if (auto* loop = wxEventLoopBase::GetActive()) loop->YieldFor(wxEVT_CATEGORY_ALL);
            worker.get();
            size_t layers = 0;
            for (const auto* item : print.objects()) for (const auto* layer : item->layers()) {
                ++layers;
                for (const auto& bounds : layer->lslices_bboxes)
                    if (std::abs(unscale<double>(bounds.size().x()) - expected_width) > .01) throw std::runtime_error("GUI-selected plugin did not modify real slice geometry");
                for (const auto* region : layer->regions())
                    if (region->perimeters.empty()) throw std::runtime_error("No native perimeters after plugin");
            }
            if (!layers) throw std::runtime_error("Empty slicing graph");
            evidence["slicing_layers"] = layers;
            evidence["slicing_width_mm"] = expected_width;
            evidence["slicing_selector_worker"] = true;
            evidence["ok"] = true;
        } else if (mode == "replacement") {
            if (scripts.size() != 1) throw std::runtime_error("Replacement smoke needs one Script");
            const auto key = scripts.front().package_key;
            const auto name = scripts.front().name;
            const auto first = runtime.execute(key, name);
            if (!first.ok || nlohmann::json::parse(first.data).at("count") != 1)
                throw std::runtime_error("Previous plugin did not run");
            PluginDescriptor candidate;
            if (!local_store().install(file.parent_path() / "broken" / file.filename(), candidate, error))
                throw std::runtime_error(error);
            if (load_installed(candidate, error)) throw std::runtime_error("Broken replacement unexpectedly loaded");
            evidence["replacement_error"] = error;
            PluginDescriptor restored;
            if (!PluginManager::instance().try_get_valid_plugin_descriptor(key, restored) || restored.version != "1.0.0" ||
                !load_installed(restored, error)) throw std::runtime_error("Previous version was not restored: " + error);
            const PluginCapabilityId id{PluginCapabilityType::Script, name, key};
            const auto config = PluginManager::instance().get_config().get_config(id);
            if (!config || config->config.at("count") != 1) throw std::runtime_error("Previous configuration was not restored");
            evidence["replacement_rollback"] = true;
            if (!local_store().install(file.parent_path() / "updated" / file.filename(), candidate, error) ||
                !load_installed(candidate, error)) throw std::runtime_error(error);
            const auto second = runtime.execute(key, name);
            if (!second.ok || nlohmann::json::parse(second.data).at("count") != 2)
                throw std::runtime_error("Successful update did not preserve configuration");
            evidence["replacement_commit"] = true;
            evidence["ok"] = true;
        } else if (mode == "config") {
            if (scripts.size() != 1)
                throw std::runtime_error("Config smoke needs one Script");
            const PluginCapabilityId id{PluginCapabilityType::Script, scripts.front().name, scripts.front().package_key};
            show_local_plugin_config(wxGetApp().mainframe, id);
            const auto stored = PluginManager::instance().get_config().get_config(id);
            if (!stored || stored->config.value("value", 0) != 42)
                throw std::runtime_error("Custom configuration did not persist");
            evidence["custom_config_saved"] = true;
            evidence["ok"] = true;
        } else if (mode == "host") {
            wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
            auto* plater = wxGetApp().plater();
            const auto model_file = std::filesystem::u8path(data_dir()) / "host-contract-cube.stl";
            auto cube = make_cube(10.0, 20.0, 30.0);
            if (!cube.write_binary(model_file.u8string().c_str()))
                throw std::runtime_error("Could not write model fixture");
            if (plater->load_files(std::vector<std::string>{model_file.u8string()}, LoadStrategy::LoadModel).empty())
                throw std::runtime_error("Could not load model fixture");
            if (scripts.size() != 1)
                throw std::runtime_error("Host smoke needs one Script");
            const auto result = runtime.execute(scripts.front().package_key, scripts.front().name);
            evidence["ok"] = result.ok;
            evidence["message"] = result.message;
            evidence["data"] = result.data;
            if (!result.ok) throw std::runtime_error(result.message);
            evidence["host_contract"] = nlohmann::json::parse(result.data);
        } else if (mode == "ui") {
            wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
            if (scripts.size() != 1)
                throw std::runtime_error("UI smoke needs one Script");
            const auto result = runtime.execute(scripts.front().package_key, scripts.front().name);
            if (!result.ok)
                throw std::runtime_error(result.message);
            const PluginCapabilityId id{PluginCapabilityType::Script, scripts.front().name, scripts.front().package_key};
            bool                     complete = false;
            const auto               deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
            while (std::chrono::steady_clock::now() < deadline) {
                if (auto* loop = wxEventLoopBase::GetActive())
                    loop->YieldFor(wxEVT_CATEGORY_ALL);
                const auto callback_error = PluginManager::instance().get_plugin_load_error(id.plugin_key);
                if (!callback_error.empty())
                    throw std::runtime_error(callback_error);
                const auto config = PluginManager::instance().get_config().get_config(id);
                if (config && config->config.value("window_roundtrip", false) && config->config.value("dock_roundtrip", false)) {
                    complete = true;
                    break;
                }
                wxMilliSleep(10);
            }
            if (!complete)
                throw std::runtime_error("Window/dock UI roundtrip did not complete");
            evidence["ui_roundtrip"] = true;
            wxMessageBox("Plugin window, dock and progress passed", "Plugin UI verification", wxOK, wxGetApp().mainframe);
            if (!runtime.unload(id.plugin_key, error))
                throw std::runtime_error(error);
            evidence["ui_removed_on_unload"] = !PluginHostUi::has_windows_for_plugin(id.plugin_key);
            if (!evidence["ui_removed_on_unload"].get<bool>())
                throw std::runtime_error("Plugin windows survived unload");
            std::atomic<bool> invoked{false}, rejected{false};
            std::thread worker([&] {
                try {
                    PythonGILState gil;
                    if (!gil) throw std::runtime_error("No Python runtime lease");
                    PluginHostUi::run_on_ui_thread([&] { invoked = true; });
                } catch (const std::exception&) { rejected = true; }
            });
            const auto pending_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (PluginHostUi::active_request_count() == 0 && !rejected && std::chrono::steady_clock::now() < pending_deadline)
                wxMilliSleep(1);
            const bool queued = PluginHostUi::active_request_count() > 0;
            PluginManager::instance().set_shutting_down();
            worker.join();
            evidence["queued_ui_cancelled_on_shutdown"] = queued && rejected && !invoked;
            if (!evidence["queued_ui_cancelled_on_shutdown"].get<bool>())
                throw std::runtime_error("Queued UI request did not cancel safely on shutdown");
            evidence["ok"] = true;
        } else if (mode == "pages") {
            auto&      manager = PluginManager::instance();
            const auto caps    = manager.get_plugin_capabilities("", PluginCapabilityType::Pages);
            if (caps.size() != 1)
                throw std::runtime_error("Page smoke requires one Pages capability");
            const auto id       = caps.front()->identity();
            bool       selected = false, complete = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
            while (std::chrono::steady_clock::now() < deadline) {
                if (auto* loop = wxEventLoopBase::GetActive())
                    loop->YieldFor(wxEVT_CATEGORY_ALL);
                if (!selected)
                    selected = select_local_plugin_page(id);
                const auto config = manager.get_config().get_config(id);
                if (config && config->config.value("roundtrip", false)) {
                    complete = true;
                    break;
                }
                wxMilliSleep(10);
            }
            if (!complete || !selected)
                throw std::runtime_error("Page bridge roundtrip did not complete");
            evidence["page_roundtrip"] = true;
            evidence["ok"]             = true;
            wxMessageBox("Plugin page roundtrip passed", "Plugin page verification", wxOK, wxGetApp().mainframe);
            if (!runtime.unload(id.plugin_key, error))
                throw std::runtime_error(error);
            evidence["page_removed_on_unload"] = !select_local_plugin_page(id);
        } else
#endif
            if (mode == "manager") {
            evidence["ok"]                 = true;
            evidence["remaining_packages"] = local_store().scan(error).size();
            if (!error.empty())
                throw std::runtime_error(error);
        } else if (mode == "disabled") {
            const auto entries = local_store().scan(error);
            if (!error.empty() || entries.size() != 1 || entries.front().has_error() || entries.front().enabled || !scripts.empty())
                throw std::runtime_error("Disabled GUI smoke expected one valid disabled package and no loaded scripts");
            evidence["ok"] = true;
        } else {
            if (scripts.size() != 1)
                throw std::runtime_error("GUI smoke fixture must register exactly one Script");
            const auto result   = runtime.execute(scripts.front().package_key, scripts.front().name);
            evidence["ok"]      = result.ok;
            evidence["message"] = result.message;
            evidence["data"]    = result.data;
            if (mode == "restart") {
                if (!local_store().set_enabled(scripts.front().package_key, false, error) ||
                    !runtime.unload(scripts.front().package_key, error))
                    throw std::runtime_error(error);
                evidence["disabled_for_next_start"] = true;
            }
        }
    } catch (const std::exception& ex) {
        evidence["ok"]    = false;
        evidence["error"] = ex.what();
    }
    if (!normal_window_close) runtime.shutdown();
    evidence["interpreter_stopped"] = !runtime.initialized();
    // These modes create disposable projects only after validating the explicit
    // isolated profile above. Clear them before the smoke launcher requests
    // Close(), so a custom save prompt cannot strand an unattended test process.
    const auto completed_mode = evidence.value("mode", std::string());
    if ((completed_mode == "workflow" || completed_mode == "texture" || completed_mode == "dualhost" || completed_mode == "legacy_states") && wxGetApp().plater()) {
        try {
            wxGetApp().plater()->new_project(true, true);
            evidence["isolated_project_cleared"] = true;
        } catch (const std::exception& ex) {
            evidence["ok"] = false;
            evidence["cleanup_error"] = ex.what();
        }
    }
    // Test fixtures also edit process settings. These belong to the validated
    // disposable profile and must not leave the normal unsaved-preset prompt
    // blocking the test application's close after all assertions have run.
    if (!completed_mode.empty()) {
        for (auto* tab : wxGetApp().tabs_list)
            if (tab && tab->get_presets())
                tab->get_presets()->discard_current_changes();
        evidence["isolated_preset_changes_discarded"] = true;
    }
    std::ofstream output(report);
    output << evidence.dump(2) << '\n';
    if (!output)
        BOOST_LOG_TRIVIAL(error) << "Could not write local plugin GUI smoke report";
}

} // namespace Slic3r::GUI
