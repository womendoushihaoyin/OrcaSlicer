// Canonical L35 GUI consumer fixture. No Python replacement, callback injection,
// synthetic browser messages, or claims about an unexecuted upstream host.
#include <GL/glew.h>
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/LocalPluginStore.hpp"
#include "slic3r/plugin/host/PluginPagesLocal.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TextureDisplacementImage.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/make_shared.hpp>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/evtloop.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/webview.h>
#include <wx/weakref.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {
namespace dual_fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void write(const dual_fs::path& path, const json& value)
{
    std::ofstream stream(path);
    stream << value.dump(2) << '\n';
    require(bool(stream), "Cannot write canonical GUI observation: " + path.u8string());
}
void pump()
{
    require(wxIsMainThread() && wxEventLoopBase::GetActive(), "Canonical consumer requires the active real GUI loop");
    wxEventLoopBase::GetActive()->YieldFor(wxEVT_CATEGORY_ALL);
    wxMilliSleep(10);
}
template<class Predicate> void wait(Predicate ready, const std::string& what, int seconds = 60)
{
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    while (!ready()) {
        require(Clock::now() < end, "Timed out waiting for " + what);
        pump();
    }
}
std::vector<json> events(const dual_fs::path& path)
{
    std::ifstream stream(path);
    std::vector<json> result;
    std::string line;
    while (std::getline(stream, line)) if (!line.empty()) result.push_back(json::parse(line));
    return result;
}
template<class T> T* descendant(wxWindow* parent)
{
    if (!parent) return nullptr;
    if (auto* value = dynamic_cast<T*>(parent)) return value;
    for (auto* child : parent->GetChildren()) if (auto* value = descendant<T>(child)) return value;
    return nullptr;
}
wxButton* button(wxWindow* parent, const wxString& label)
{
    if (auto* value = dynamic_cast<wxButton*>(parent); value && value->GetLabel() == label) return value;
    for (auto* child : parent->GetChildren()) if (auto* value = button(child, label)) return value;
    return nullptr;
}
wxChoice* choice_with(wxWindow* parent, const wxString& label)
{
    if (auto* value = dynamic_cast<wxChoice*>(parent); value && value->FindString(label, true) != wxNOT_FOUND) return value;
    for (auto* child : parent->GetChildren()) if (auto* value = choice_with(child, label)) return value;
    return nullptr;
}
void press(wxWindow* parent, const wxString& label)
{
    auto* target = button(parent, label);
    require(target && target->IsEnabled(), "Required editor button unavailable: " + std::string(label.ToUTF8()));
    wxCommandEvent event(wxEVT_BUTTON, target->GetId());
    event.SetEventObject(target);
    require(target->GetEventHandler()->ProcessEvent(event), "Editor did not handle its actual button event");
}

// Observe the actual front buffer after the production preview has finished.
// No scene, GL model, or shader is constructed by this fixture.
json capture_preview(wxGLCanvas& canvas, const dual_fs::path& output)
{
    require(canvas.IsShownOnScreen(), "Texture preview is not visible");
    auto* context = wxGetApp().init_glcontext(canvas);
    require(context && canvas.SetCurrent(*context), "Cannot access the real preview GL context");
    require(glGetError() == GL_NO_ERROR, "Production preview left an OpenGL error");
    GLint viewport[4]; glGetIntegerv(GL_VIEWPORT, viewport);
    const int width = viewport[2], height = viewport[3];
    require(width >= 100 && height >= 100 && width <= 8192 && height <= 8192, "Unexpected GL preview dimensions");
    GLint read_buffer = 0, packing = 0;
    glGetIntegerv(GL_READ_BUFFER, &read_buffer); glGetIntegerv(GL_PACK_ALIGNMENT, &packing);
    glReadBuffer(GL_FRONT); // Read the displayed buffer in both single/double-buffer modes.
    GLint row_length = 0, skip_rows = 0, skip_pixels = 0;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length); glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows); glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<unsigned char> pixels(size_t(width) * height * 3);
    glFinish(); glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    const unsigned error = glGetError();
    glReadBuffer(read_buffer); glPixelStorei(GL_PACK_ALIGNMENT, packing);
    glPixelStorei(GL_PACK_ROW_LENGTH, row_length); glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows); glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
    require(error == GL_NO_ERROR, "Actual preview framebuffer read failed: " + std::to_string(error));
    size_t different = 0;
    for (size_t index = 3; index < pixels.size(); index += 3)
        if (std::abs(int(pixels[index]) - pixels[0]) + std::abs(int(pixels[index+1]) - pixels[1]) +
            std::abs(int(pixels[index+2]) - pixels[2]) > 24) ++different;
    require(different > 100, "Actual GL preview contains only its clear color");
    wxImage image(width, height, pixels.data(), true);
    require(image.Mirror(false).SaveFile(wxString::FromUTF8(output.u8string()), wxBITMAP_TYPE_PNG), "Cannot save real GL preview PNG");
    return {{"width", width}, {"height", height}, {"non_background_pixels", different}, {"gl_error", error}};
}

// Only public wx controls are used. Their production handlers run under the
// actual dialog's nested modal loop, including its asynchronous preview job.
class TextureButtons final : public wxTimer
{
public:
    TextureButtons(std::string selected, dual_fs::path preview) : m_selected(std::move(selected)), m_preview(std::move(preview)) { Start(100); }
    ~TextureButtons() override { Stop(); }
    void Notify() override
    {
        if (m_busy) return;
        wxDialog* dialog = nullptr;
        for (auto* window : wxTopLevelWindows)
            if (auto* value = dynamic_cast<wxDialog*>(window); value && value->IsModal() && value->GetTitle() == _L("Edit texture layers")) {
                dialog = value; break;
            }
        if (!dialog) return;
        m_busy = true;
        try {
            require(Clock::now() - m_start < std::chrono::seconds(120), "Texture dialog did not complete within 120 seconds");
            if (m_step == 0) {
                auto* library = choice_with(dialog, wxString::FromUTF8(m_selected));
                require(library != nullptr, "Canonical plugin image is missing from the actual editor picker");
                library->SetSelection(library->FindString(wxString::FromUTF8(m_selected), true));
                press(dialog, _L("Add layer"));
                ++m_step;
            } else if (m_step == 1) {
                press(dialog, _L("Paint all")); ++m_step;
            } else if (m_step == 2) {
                press(dialog, _L("Preview displacement")); ++m_step;
            } else if (m_step == 3) {
                auto* bake = button(dialog, _L("Bake into mesh"));
                if (bake && bake->IsEnabled()) {
                    auto* canvas = descendant<wxGLCanvas>(dialog);
                    require(canvas != nullptr, "Editor has no actual GL canvas");
                    canvas->Refresh(false); canvas->Update();
                    // Let the posted paint complete on another modal-loop turn.
                    m_ready = Clock::now(); ++m_step;
                }
            } else if (m_step == 4 && Clock::now() - m_ready > std::chrono::milliseconds(250)) {
                auto* canvas = descendant<wxGLCanvas>(dialog);
                require(canvas != nullptr, "Editor GL canvas disappeared");
                preview = capture_preview(*canvas, m_preview);
                preview["actual_editor_buttons"] = {"Add layer", "Paint all", "Preview displacement", "Bake into mesh"};
                Stop(); press(dialog, _L("Bake into mesh")); completed = true;
            }
        } catch (const std::exception& error) {
            failure = error.what(); Stop();
            // Use the normal cancellation path so an outstanding worker drains.
            try { press(dialog, _L("Cancel")); } catch (...) { dialog->Close(); }
        }
        m_busy = false;
    }
    json preview;
    bool completed = false;
    std::string failure;
private:
    std::string m_selected;
    dual_fs::path m_preview;
    Clock::time_point m_start = Clock::now(), m_ready;
    int m_step = 0;
    bool m_busy = false;
};

void select_presets()
{
    auto& bundle = *wxGetApp().preset_bundle;
    const std::string printer = "Snapmaker U1 (0.4 nozzle)", process = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)",
                      filament = "Snapmaker PLA Basic @U1";
    wxGetApp().app_config->set_variant("Snapmaker", "Snapmaker U1", "0.4", true);
    if (!bundle.printers.find_preset(printer) || !bundle.prints.find_preset(process) || !bundle.filaments.find_preset(filament))
        bundle.load_presets(*wxGetApp().app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
    require(bundle.printers.find_preset(printer) && bundle.prints.find_preset(process) && bundle.filaments.find_preset(filament), "Bundled U1/PLA presets unavailable");
    bundle.printers.find_preset(printer, false, true)->is_visible = true;
    bundle.prints.find_preset(process, false, true)->is_visible = true;
    bundle.filaments.find_preset(filament, false, true)->is_visible = true;
    require(bundle.printers.select_preset_by_name(printer, true) && bundle.prints.select_preset_by_name(process, true) &&
            bundle.filaments.select_preset_by_name(filament, true), "Could not select real texture-consumer presets");
    bundle.filament_presets = {filament}; bundle.update_multi_material_filament_presets(); bundle.set_filament_preset(0, filament);
    auto& config = bundle.prints.get_edited_preset().config;
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings());
    config.set_key_value("plugins", new ConfigOptionStrings());
    config.set_key_value("print_plugin_config_overrides", new ConfigOptionString("[]"));
    wxGetApp().load_current_presets(false, false);
    wxGetApp().plater()->on_config_change(bundle.full_config());
}

struct Runner {
    json request, observation, observations = json::object();
    std::vector<std::string> kinds;
    std::string label, kind, form, phase;
    dual_fs::path packages, output, storage, checkpoint;
    PluginManager& manager = PluginManager::instance();
    PluginDescriptor descriptor;
    PluginCapabilityId id;
    size_t exercise_count = 0;
    std::vector<TextureLibraryEntry> previous_entries;
    void select_kind(const std::string& selected)
    {
        if (!kind.empty()) observations[kind] = observation;
        kind = selected; observation = observations.at(kind);
        id = {kind == "pages" ? PluginCapabilityType::Pages : PluginCapabilityType::ResourceTexture,
              kind == "pages" ? "Dualhost Page" : "Dualhost Texture", descriptor.plugin_key};
    }
    json result()
    {
        if (!kind.empty()) observations[kind] = observation;
        bool passed = !observations.empty();
        for (const auto& value : observations.items()) passed &= value.value().value("passed", false);
        return {{"passed", passed}, {"cases", observations}, {"shared_package_lifecycle", kinds.size() > 1}};
    }
    void operation(const char* action, const std::function<void()>& body, bool expected_error = false, bool per_capability = false)
    {
        // Multi's package operations happen once. Each affected capability gets
        // the same actual operation/PID; exercise/config/consumer checks run once
        // per capability and retain separate errors, traces and evidence.
        const auto targets = per_capability ? kinds : std::vector<std::string>{kinds.front()};
        for (const auto& target : targets) {
            select_kind(target);
            bool failed = false;
            try { body(); }
            catch (const std::exception& error) {
                failed = true;
                observation["errors"].push_back({{"type", "std::runtime_error"}, {"operation", action}, {"message", error.what()}});
                observation["operations"].push_back({{"action", action}, {"outcome", "error"}, {"pid", static_cast<long>(wxGetProcessId())}});
                write(checkpoint, result());
                if (!expected_error) throw;
            }
            if (!failed) observation["operations"].push_back({{"action", action}, {"outcome", "ok"}, {"pid", static_cast<long>(wxGetProcessId())}});
            if (!per_capability) {
                observations[kind] = observation;
                for (const auto& other : kinds) if (other != kind) {
                    auto& mirrored = observations[other];
                    mirrored["operations"].push_back(observation["operations"].back());
                    for (const char* key : {"storage", "package_key", "capability_enabled", "registered_capabilities", "archived_traces"})
                        if (observation.contains(key)) mirrored[key] = observation[key];
                }
            }
            write(checkpoint, result());
            require(!expected_error || failed, "Deliberate error was accepted as success");
        }
    }
    dual_fs::path wheel() const { return packages / ("orca_dualhost_" + label + "-1.0.0-py3-none-any.whl"); }
    void resolve()
    {
        if (descriptor.plugin_key.empty()) {
            const auto descriptors = manager.get_plugin_descriptors();
            require(descriptors.size() == 1, "Expected one canonical package in this isolated GUI profile");
            descriptor = descriptors.front();
        }
        id = {kind == "pages" ? PluginCapabilityType::Pages : PluginCapabilityType::ResourceTexture,
              kind == "pages" ? "Dualhost Page" : "Dualhost Texture", descriptor.plugin_key};
        storage = dual_fs::u8path(manager.get_storage_dir(descriptor.plugin_key));
        observation["storage"] = storage.u8string(); observation["package_key"] = descriptor.plugin_key;
    }
    void install()
    {
        std::string error;
        require(manager.install_plugin(wheel().u8string(), descriptor, error), "Canonical wheel installation failed: " + error);
        manager.rescan_plugins(); resolve();
    }
    void enable(bool enabled)
    {
        if (!enabled && manager.is_plugin_loaded(descriptor.plugin_key))
            require(manager.unload_plugin(descriptor.plugin_key), "Cannot unload before disabling canonical package");
        std::string error;
        LocalPluginStore store(dual_fs::u8path(get_orca_plugins_dir()), {});
        require(store.set_enabled(descriptor.plugin_key, enabled, error), error);
        observation["capability_enabled"] = enabled;
    }
    void load()
    {
        manager.load_plugin(descriptor.plugin_key, false);
        wait([&] { return !manager.is_plugin_load_in_progress(descriptor.plugin_key); }, "canonical plugin load");
        require(manager.is_plugin_loaded(descriptor.plugin_key) && bool(manager.get_plugin_capability(id)),
                "Canonical capability failed to load: " + manager.get_plugin_load_error(descriptor.plugin_key));
        observation["capability_enabled"] = true;
        observation["registered_capabilities"] = manager.get_plugin_capabilities(descriptor.plugin_key).size();
        require(observation["registered_capabilities"] == (label == "multi" ? 4 : 1), "Canonical package did not register all declared capabilities");
        if (kind == "texture") wait([&] {
            const auto entries = texture_library(true);
            return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.plugin_key == descriptor.plugin_key; });
        }, "real texture provider observer");
    }
    void configuration(int token, const char* mode)
    {
        require(manager.get_config().store_capability_config(id, {{"token", token}, {"mode", mode}}), "Cannot persist canonical configuration");
    }
    void page(bool expect_error)
    {
        wxWeakRef<wxWebView> view;
        wait([&] {
            if (!select_local_plugin_page(id)) return false;
            view = descendant<wxWebView>(wxGetApp().tab_panel()->GetCurrentPage());
            if (!view || view->IsBusy()) return false;
            wxString ready;
            return view->RunScript("Boolean(window.orca && document.getElementById('ping') && document.getElementById('ping').onclick)", &ready) && ready == "true";
        }, "canonical actual WebView and registered JavaScript handler");
        const auto trace = storage / "dualhost-events.jsonl";
        const auto before = events(trace).size();
        using Sink = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
        auto messages = boost::make_shared<std::ostringstream>();
        auto sink = boost::make_shared<Sink>();
        sink->locked_backend()->add_stream(messages);
        sink->set_formatter(boost::log::expressions::stream << boost::log::expressions::smessage);
        boost::log::core::get()->add_sink(sink);
        struct RemoveSink { boost::shared_ptr<Sink> value; ~RemoveSink() { boost::log::core::get()->remove_sink(value); } } remove{sink};
        // This executes the unmodified wheel's onclick handler in the real browser.
        // All bridge messages, replies and acknowledgements are generated there.
        require(view->RunScript("document.getElementById('ping').click();"), "Canonical JavaScript click failed");
        json event;
        wait([&] {
            const auto current = events(trace);
            for (size_t index = before; index < current.size(); ++index)
                if (current[index]["capability"] == "pages" && current[index]["event"] == (expect_error ? "error" : "page_ack")) {
                    event = current[index]; return true;
                }
            return false;
        }, expect_error ? "actual Python Pages exception" : "actual JavaScript reply acknowledgement");
        if (expect_error) {
            wait([&] { return messages->str().find("Plugin page message failed:") != std::string::npos; }, "host error diagnostic");
            require(messages->str().find("dualhost deliberate page exception") != std::string::npos, "Host did not log the canonical Python exception");
            observation["page_host_error"] = messages->str();
            observation["errors"].push_back(event.at("payload"));
            throw std::runtime_error(event.at("payload").at("message").get<std::string>());
        }
        const json expected{{"reply", {{"text", "中文-α"}, {"items", {true, 2.5, nullptr}}}}, {"token", form == "config-restart" ? 91 : 37}};
        require(event.at("payload") == expected, "Canonical JS/Python roundtrip changed values or types");
        wxString actual;
        require(view && view->RunScript("document.getElementById('result').textContent", &actual), "Cannot inspect real DOM reply");
        require(json::parse(std::string(actual.ToUTF8())) == expected, "Actual DOM reply differs from Python acknowledgement");
        observation["page_roundtrips"].push_back({{"payload", expected}, {"pid", static_cast<long>(wxGetProcessId())}, {"real_dom", true}});
        const auto dom = output / (phase + "-page-" + std::to_string(exercise_count) + ".json");
        write(dom, {{"dom_result", expected}, {"source", "wxWebView.RunScript document.getElementById('result').textContent"}});
        observation["artifacts"]["page_dom"] = dom.u8string();
    }
    std::vector<TextureLibraryEntry> entries()
    {
        wait([&] {
            const auto active = texture_library(true);
            return std::any_of(active.begin(), active.end(), [&](const auto& item) { return item.plugin_key == descriptor.plugin_key; });
        }, "current texture provider observer");
        std::vector<TextureLibraryEntry> result;
        for (const auto& entry : texture_library(true))
            if (entry.source == TextureSource::Plugin && entry.plugin_key == descriptor.plugin_key && entry.capability_name == id.name)
                result.push_back(entry);
        require(result.size() == 2, "Canonical texture provider must expose valid and invalid PNG entries");
        return result;
    }
    void texture(bool expect_error)
    {
        wait([&] {
            const auto active = texture_library(true);
            return std::any_of(active.begin(), active.end(), [&](const auto& item) { return item.plugin_key == descriptor.plugin_key; });
        }, "current texture provider observer");
        const auto available = entries(); previous_entries = available;
        const auto filename = expect_error ? "bad-image.png" : "height-纹理.png";
        const auto found = std::find_if(available.begin(), available.end(), [&](const auto& item) { return dual_fs::u8path(item.path).filename().u8string() == filename; });
        require(found != available.end(), "Canonical provider image missing");
        std::string error;
        std::shared_ptr<std::vector<unsigned char>> image;
        if (expect_error) {
            // wxImage also queues a generic GUI log dialog. This deliberate bad
            // PNG case retains the consumer's actual error without stranding an
            // unattended run behind an unrelated logger popup.
            wxLogNull expected_image_warning;
            image = load_texture_image_data(*found, error);
        } else image = load_texture_image_data(*found, error);
        if (expect_error) {
            require(!image && !error.empty(), "Invalid canonical PNG was not rejected by the production resource consumer");
            observation["resource_consumer"]["invalid_image_rejected"] = true;
            observation["errors"].push_back({{"type", "TextureImageDecodeError"}, {"message", error}});
            throw std::runtime_error(error);
        }
        require(image && !image->empty() && error.empty(), "Canonical texture decode failed: " + error);
        TextureDisplacementLayer decoded_layer; decoded_layer.image_data = image;
        require(!decode_height_texture(decoded_layer).empty(), "Canonical decoded image contains no height pixels");
        auto* plater = wxGetApp().plater();
        wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
        wxGetApp().app_config->set("background_processing", "0");
        plater->new_project(true, true, "Canonical resource texture"); select_presets();
        const auto mesh = packages / "reference-cube.stl";
        require(!plater->load_files(std::vector<std::string>{mesh.u8string()}, LoadStrategy::LoadModel).empty(), "Canonical STL failed actual Plater import");
        wait([&] { return plater->model().objects.size() == 1 && !plater->model().objects.front()->volumes.empty(); }, "canonical Plater geometry");
        const auto volume = [&]() -> ModelVolume& { require(plater->model().objects.size() == 1 && plater->model().objects.front()->volumes.size() == 1, "Canonical model structure changed"); return *plater->model().objects.front()->volumes.front(); };
        const auto original = volume().mesh().its;
        const double original_volume = its_volume(original);
        plater->select_all();
        const auto stem = phase + "-texture-" + std::to_string(exercise_count);
        const auto preview = output / (stem + ".png");
        TextureButtons controls(found->pack_name + " / " + found->name, preview);
        plater->apply_resource_texture();
        controls.Stop();
        require(controls.failure.empty(), controls.failure);
        require(controls.completed, "Canonical texture editor closed before Preview/Bake");
        const auto baked = volume().mesh().its;
        require(baked.indices.size() > original.indices.size() && its_volume(baked) > original_volume + .001,
                "Production resource consumer did not apply measurable relief");
        require(volume().texture_displacement_layers.size() == 1, "Bake lost the editable texture layer");
        const auto& layer = volume().texture_displacement_layers.front();
        require(layer.image_data && *layer.image_data == *image && dual_fs::u8path(layer.path) == dual_fs::u8path(found->path),
                "Actual editor consumed another image or changed canonical pixels");
        const auto bytes = *layer.image_data;
        const double baked_volume = its_volume(baked);
        plater->undo();
        require(volume().mesh().its.vertices == original.vertices && volume().mesh().its.indices == original.indices &&
                volume().texture_displacement_layers.empty(), "Formal Undo failed to restore canonical source geometry");
        plater->redo();
        require(volume().mesh().its.vertices == baked.vertices && volume().mesh().its.indices == baked.indices,
                "Formal Redo failed to restore canonical baked geometry");
        const auto project = output / (stem + ".3mf");
        require(plater->export_3mf_copy(boost::filesystem::path(project.u8string())), "Production 3MF export failed");
        plater->new_project(true, true, "Canonical 3MF roundtrip");
        require(!plater->load_files(std::vector<std::string>{project.u8string()}, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence).empty(), "Production 3MF reload failed");
        // BBS 3MF re-centers local vertices. Compare topology/volume and owned
        // image data here; do not mistake equivalent local coordinates for loss.
        const auto& reopened = volume().mesh().its;
        require(reopened.indices == baked.indices && reopened.vertices.size() == baked.vertices.size() &&
                std::abs(its_volume(reopened) - baked_volume) < std::max(.001, baked_volume * 1e-5),
                "Production 3MF roundtrip changed canonical relief");
        const Vec3f recenter = reopened.vertices.front() - baked.vertices.front();
        double vertex_error = 0.;
        for (size_t index = 0; index < baked.vertices.size(); ++index)
            vertex_error = std::max(vertex_error, double((reopened.vertices[index] - baked.vertices[index] - recenter).norm()));
        require(vertex_error < 1e-4, "3MF vertex changes exceeded its uniform local recentering");
        require(volume().texture_displacement_layers.size() == 1 && volume().texture_displacement_layers.front().image_data &&
                *volume().texture_displacement_layers.front().image_data == bytes, "3MF lost the consumed plugin image");
        auto& consumer = observation["resource_consumer"];
        for (const auto* key : {"image_decoded", "preview_rendered", "mesh_changed", "undo_restored", "project_roundtrip"}) consumer[key] = true;
        consumer["exercises"].push_back({{"preview", controls.preview}, {"original_triangles", original.indices.size()},
            {"baked_triangles", baked.indices.size()}, {"original_volume", original_volume}, {"baked_volume", baked_volume}, {"image_bytes", bytes.size()}, {"roundtrip_vertex_error_mm", vertex_error}});
        observation["artifacts"]["mesh"] = mesh.u8string(); observation["artifacts"]["preview"] = preview.u8string(); observation["artifacts"]["project"] = project.u8string();
        plater->new_project(true, true, "Canonical consumer complete");
    }
    void exercise(bool error = false) { ++exercise_count; if (kind == "pages") page(error); else texture(error); }
    void verify_disabled()
    {
        require(!manager.is_plugin_loaded(descriptor.plugin_key) && !manager.get_plugin_capability(id), "Disabled canonical capability is live");
        const auto enabled = manager.get_enabled_plugin_keys();
        require(std::find(enabled.begin(), enabled.end(), descriptor.plugin_key) == enabled.end(), "Disabled canonical package still autoloads");
        wait([&] {
            if (kind == "pages") return !select_local_plugin_page(id);
            const auto active = texture_library(true);
            return std::none_of(active.begin(), active.end(), [&](const auto& item) { return item.plugin_key == descriptor.plugin_key; });
        }, "consumer removal after disable");
        for (const auto& entry : previous_entries) {
            std::string error; require(!load_texture_image_data(entry, error) && !error.empty(), "Old texture picker entry survived disable/unload");
        }
        observation["capability_enabled"] = false;
    }
    void run()
    {
        require(request.at("schema") == 1 && request.at("protocol") == "local-dualhost-v1", "Unknown canonical GUI request");
        label = request.at("label"); kinds = request.at("capabilities").get<std::vector<std::string>>(); form = request.at("form"); phase = request.at("phase");
        require(kinds == std::vector<std::string>{"pages"} || kinds == std::vector<std::string>{"texture"} ||
                (label == "multi" && kinds == std::vector<std::string>{"pages", "texture"}), "Invalid canonical GUI capability group");
        require(label == "multi" || (kinds.size() == 1 && label == kinds.front()), "Canonical package/capability mismatch");
        require(phase == "first" || phase == "resume", "Invalid canonical process phase");
        packages = dual_fs::u8path(request.at("packages").get<std::string>()); output = dual_fs::u8path(request.at("output").get<std::string>());
        require(dual_fs::weakly_canonical(dual_fs::u8path(request.at("profile").get<std::string>())) == dual_fs::weakly_canonical(dual_fs::u8path(data_dir())), "Canonical request/profile mismatch");
        for (const auto& selected : kinds) {
        observations[selected] = {{"execution", "gui-host"}, {"case", label + "/" + selected + "/" + form}, {"phase", phase},
            {"operations", json::array()}, {"configuration", json::object()}, {"errors", json::array()}, {"returns", json::array()},
            {"resource_consumer", json::object()}, {"artifacts", json::object()}, {"archived_traces", json::array()}, {"page_roundtrips", json::array()},
            {"expected_token", form == "config-restart" ? 91 : 37}, {"passed", false}};
        }
        select_kind(kinds.front());
        manager.rescan_plugins();
        if (phase == "resume") {
            require(form == "installed-disabled" || form == "config-restart", "This form has no second process");
            resolve(); operation("restart", [&] { if (form == "config-restart") load(); });
            if (form == "installed-disabled") operation("verify_disabled", [&] { verify_disabled(); }, false, true);
            else {
                operation("verify_config", [&] {
                    const auto saved = manager.get_config().get_config(id);
                    require(saved && saved->config == json({{"token", 91}, {"mode", "success"}}), "Canonical configuration did not persist across process restart");
                    observation["configuration"]["after_restart"] = saved->config;
                }, false, true);
                operation("exercise", [&] { exercise(); }, false, true);
            }
        } else {
            require(manager.get_plugin_descriptors().empty(), "First canonical launch requires a fresh isolated profile");
            operation("install", [&] { install(); });
            if (form == "installed-disabled") operation("disable", [&] { enable(false); });
            else {
                operation("enable", [&] { enable(true); }); operation("load", [&] { load(); });
                if (form == "config-restart") operation("set_config", [&] { configuration(91, "success"); }, false, true);
                if (form != "error-recovery") operation("exercise", [&] { exercise(); }, false, true);
                if (form == "disable-reenable") {
                    operation("disable", [&] { enable(false); }); operation("verify_disabled", [&] { verify_disabled(); }, false, true);
                    operation("enable", [&] { enable(true); load(); }); operation("exercise", [&] { exercise(); }, false, true);
                } else if (form == "same-package-reload") {
                    operation("reload", [&] { require(manager.unload_plugin(descriptor.plugin_key), "Canonical reload unload failed"); load(); });
                    operation("exercise", [&] { exercise(); }, false, true);
                } else if (form == "uninstall-reinstall") {
                    operation("uninstall", [&] {
                        require(manager.unload_plugin(descriptor.plugin_key), "Canonical uninstall unload failed");
                        const auto trace = output / "before-uninstall.jsonl";
                        dual_fs::copy_file(storage / "dualhost-events.jsonl", trace);
                        observation["archived_traces"].push_back(trace.u8string());
                        dual_fs::remove(storage / "dualhost-events.jsonl"); // Rotate only the captured log, after unload drained.
                        std::string error; require(manager.delete_plugin(descriptor.plugin_key, error), error);
                    });
                    operation("verify_uninstalled", [&] {
                        PluginDescriptor stale;
                        require(!manager.try_get_plugin_descriptor(descriptor.plugin_key, stale) && !dual_fs::exists(dual_fs::u8path(descriptor.plugin_root)), "Canonical uninstall left registered/on-disk package");
                    });
                    operation("install", [&] { install(); }); operation("enable", [&] { enable(true); }); operation("load", [&] { load(); });
                    operation("exercise", [&] { exercise(); }, false, true);
                } else if (form == "error-recovery") {
                    operation("set_error", [&] { if (kind == "pages") configuration(37, "raise"); else (void) entries(); }, false, true);
                    operation("exercise_error", [&] { exercise(true); }, true, true);
                    operation("recover", [&] { if (kind == "pages") configuration(37, "success"); }, false, true); operation("exercise", [&] { exercise(); }, false, true);
                } else require(form == "fresh-load" || form == "config-restart", "Unknown canonical form");
            }
        }
        observations[kind] = observation;
        for (auto& value : observations.items()) {
            value.value()["awaiting_normal_close"] = true;
            value.value()["passed"] = true;
        }
        observation = observations.at(kind);
        // The external finalizer adds close only after real OnExit confirms that
        // runtime shutdown completed in this PID. Merely returning is not close.
        write(checkpoint, result());
    }
};
} // namespace

nlohmann::json run_plugin_dualhost_gui(const std::filesystem::path& request_path, const std::filesystem::path& report)
{
    require(wxIsMainThread() && wxEventLoopBase::GetActive(), "Canonical fixture requires the actual GUI thread");
    wxString mode, isolated;
    require(wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_MODE", &mode) && mode == "dualhost" &&
            wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_DATADIR", &isolated) && !isolated.empty() &&
            dual_fs::weakly_canonical(dual_fs::u8path(isolated.ToUTF8().data())) == dual_fs::weakly_canonical(dual_fs::u8path(data_dir())),
            "Canonical fixture refused the active user profile");
    Runner runner; runner.checkpoint = dual_fs::path(report).concat(".dualhost");
    std::ifstream input(request_path); require(bool(input), "Canonical request missing"); input >> runner.request;
    try { runner.run(); }
    catch (const std::exception& error) { runner.observation["passed"] = false; runner.observation["fatal_error"] = error.what(); write(runner.checkpoint, runner.result()); }
    if (wxGetApp().plater()) wxGetApp().plater()->new_project(true, true, "Canonical GUI cleanup");
    return runner.result();
}
} // namespace Slic3r::GUI
