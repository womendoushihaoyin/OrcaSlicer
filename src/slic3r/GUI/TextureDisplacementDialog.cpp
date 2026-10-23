#include <GL/glew.h>

#include "TextureDisplacementDialog.hpp"
#include "TextureLibrary.hpp"
#include "PluginWorkflowSmoke.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GLModel.hpp"
#include "GLShader.hpp"
#include "OpenGLManager.hpp"
#include "I18N.hpp"
#include "libslic3r/TextureDisplacementConsumer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcclient.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/glcanvas.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/timer.h>

namespace Slic3r::GUI {
namespace {

class TextureMeshPreview final : public wxPanel
{
public:
    TextureMeshPreview(wxWindow* parent, std::shared_ptr<const indexed_triangle_set> mesh)
        : wxPanel(parent), m_mesh(std::move(mesh))
    {
        m_canvas = OpenGLManager::create_wxglcanvas(*this);
        m_context = wxGetApp().init_glcontext(*m_canvas);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(m_canvas, 1, wxEXPAND);
        SetSizer(layout);
        SetMinSize(wxSize(480, 420));
        Vec3f lo = m_mesh->vertices.front(), hi = lo;
        for (const auto& p : m_mesh->vertices) { lo = lo.cwiseMin(p); hi = hi.cwiseMax(p); }
        m_center = ((lo + hi) * .5f).cast<double>();
        m_radius = std::max(0.01, double((hi - lo).norm()) * .65);
        m_canvas->Bind(wxEVT_PAINT, &TextureMeshPreview::paint, this);
        m_canvas->Bind(wxEVT_SIZE, &TextureMeshPreview::resize, this);
        m_canvas->Bind(wxEVT_LEFT_DOWN, &TextureMeshPreview::mouse_down, this);
        m_canvas->Bind(wxEVT_LEFT_UP, &TextureMeshPreview::mouse_up, this);
        m_canvas->Bind(wxEVT_MOTION, &TextureMeshPreview::mouse_move, this);
        m_canvas->Bind(wxEVT_MOUSEWHEEL, &TextureMeshPreview::wheel, this);
        m_canvas->Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});
    }

    ~TextureMeshPreview() override
    {
        // Both replacement and destruction release VBO/IBO/VAO while this canvas's context is current.
        if (m_canvas && m_context) {
            m_canvas->SetCurrent(*m_context);
            m_model.reset();
            if (m_canvas->HasCapture())
                m_canvas->ReleaseMouse();
        }
    }

    void set_mesh(std::shared_ptr<const indexed_triangle_set> mesh)
    {
        m_mesh = std::move(mesh);
        m_dirty = true;
        m_canvas->Refresh(false);
    }

private:
    void resize(wxSizeEvent& event) { m_canvas->Refresh(false); event.Skip(); }
    void mouse_down(wxMouseEvent& event)
    {
        m_last = event.GetPosition();
        if (!m_canvas->HasCapture())
            m_canvas->CaptureMouse();
    }
    void mouse_up(wxMouseEvent&)
    {
        if (m_canvas->HasCapture())
            m_canvas->ReleaseMouse();
    }
    void mouse_move(wxMouseEvent& event)
    {
        if (!event.Dragging() || !event.LeftIsDown())
            return;
        const auto p = event.GetPosition();
        m_yaw += double(p.x - m_last.x) * .01;
        m_pitch = std::clamp(m_pitch + double(p.y - m_last.y) * .01, -1.5, 1.5);
        m_last = p;
        m_canvas->Refresh(false);
    }
    void wheel(wxMouseEvent& event)
    {
        m_zoom = std::clamp(m_zoom * (event.GetWheelRotation() > 0 ? 1.1 : 1. / 1.1), .2, 5.);
        m_canvas->Refresh(false);
    }
    void paint(wxPaintEvent&)
    {
        wxPaintDC dc(m_canvas);
        if (!m_canvas->IsShownOnScreen() || !m_context || !m_canvas->SetCurrent(*m_context) || !wxGetApp().init_opengl())
            return;
        const wxSize size = m_canvas->GetClientSize();
        const double pixel_scale = m_canvas->GetContentScaleFactor();
        const int width = std::max(1, int(size.x * pixel_scale)), height = std::max(1, int(size.y * pixel_scale));
        glViewport(0, 0, width, height);
        glClearColor(.12f, .13f, .15f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        if (m_dirty) {
            m_model.reset();
            m_model.init_from(*m_mesh);
            m_model.set_color(ColorRGBA(.3f, .72f, .92f, 1.f));
            m_dirty = false;
        }
        bool rendered = false;
        if (auto* shader = wxGetApp().get_shader("gouraud_light")) {
            Transform3d view = Transform3d::Identity();
            view.linear() = (Eigen::AngleAxisd(m_pitch, Vec3d::UnitX()) * Eigen::AngleAxisd(m_yaw, Vec3d::UnitZ())).toRotationMatrix();
            view.translation() = -view.linear() * m_center;
            Transform3d projection = Transform3d::Identity();
            const double aspect = double(width) / double(height);
            projection.matrix()(0, 0) = m_zoom / (m_radius * std::max(1., aspect));
            projection.matrix()(1, 1) = m_zoom / (m_radius * std::max(1., 1. / aspect));
            projection.matrix()(2, 2) = -1. / (m_radius * 4.);
            shader->start_using();
            shader->set_uniform("emission_factor", .05f);
            shader->set_uniform("view_model_matrix", view);
            shader->set_uniform("projection_matrix", projection);
            shader->set_uniform("view_normal_matrix", Matrix3d(view.linear()));
            m_model.render();
            rendered = true;
            shader->stop_using();
        }
        m_canvas->SwapBuffers();
        if (plugin_texture_smoke_active())
            record_plugin_texture_preview(m_mesh->indices.size(), rendered, glGetError());
    }

    wxGLCanvas* m_canvas = nullptr;
    wxGLContext* m_context = nullptr; // application-owned; the modal dialog closes before app teardown
    GLModel m_model;
    std::shared_ptr<const indexed_triangle_set> m_mesh;
    Vec3d m_center = Vec3d::Zero();
    double m_radius = 1., m_yaw = -.6, m_pitch = -.9, m_zoom = 1.;
    wxPoint m_last;
    bool m_dirty = true;
};

struct BakeJobState
{
    std::atomic<bool> canceled{false};
    std::atomic<int> stage_progress{0};
};

class TextureDisplacementDialog final : public wxDialog
{
public:
    TextureDisplacementDialog(wxWindow* parent, indexed_triangle_set source, bool flip,
                              std::function<bool()> source_is_current)
        : wxDialog(parent, wxID_ANY, _L("Texture displacement"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_source(std::make_shared<const indexed_triangle_set>(std::move(source))), m_flip(flip),
          m_source_is_current(std::move(source_is_current)), m_timer(this)
    {
        auto* vertical = new wxBoxSizer(wxVERTICAL);
        auto* content = new wxBoxSizer(wxHORIZONTAL);
        auto* settings = new wxBoxSizer(wxVERTICAL);
        settings->Add(new wxStaticText(this, wxID_ANY, _L("Texture")), 0, wxBOTTOM, 5);
        m_choice = new wxChoice(this, wxID_ANY);
        m_choice->SetMinSize(wxSize(270, -1));
        settings->Add(m_choice, 0, wxEXPAND | wxBOTTOM, 8);
        auto* import = new wxButton(this, wxID_ANY, _L("Import image"));
        settings->Add(import, 0, wxEXPAND | wxBOTTOM, 16);
        m_depth = parameter(settings, _L("Depth (mm)"), 0., 5., .4, .1);
        m_scale = parameter(settings, _L("Tile size (mm)"), .1, 1000., 12.5, .5);
        m_rotation = parameter(settings, _L("Rotation (degrees)"), -180., 180., 0., 5.);
        m_edge = parameter(settings, _L("Mesh resolution (mm)"), .05, 5., .5, .05);
        m_invert = new wxCheckBox(this, wxID_ANY, _L("Invert height"));
        settings->Add(m_invert, 0, wxTOP | wxBOTTOM, 8);
        auto* note = new wxStaticText(this, wxID_ANY,
            _L("Applying replaces the selected part with the baked mesh and clears its support, seam, color and fuzzy-skin painting. You can undo this change. Save the project to keep the result.\nDrag to rotate; scroll to zoom."));
        note->Wrap(270);
        settings->Add(note, 0, wxTOP | wxBOTTOM, 10);
        m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_status->Wrap(270);
        settings->Add(m_status, 1, wxEXPAND | wxTOP, 8);
        content->Add(settings, 0, wxALL | wxEXPAND, 12);
        m_preview = new TextureMeshPreview(this, m_source);
        content->Add(m_preview, 1, wxALL | wxEXPAND, 12);
        vertical->Add(content, 1, wxEXPAND);
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        m_apply = new wxButton(this, wxID_OK, _L("Apply baked mesh"));
        m_apply->Disable();
        buttons->AddStretchSpacer();
        buttons->Add(cancel, 0, wxRIGHT, 8);
        buttons->Add(m_apply);
        vertical->Add(buttons, 0, wxALL | wxEXPAND, 12);
        SetSizerAndFit(vertical);
        SetMinSize(wxSize(800, 580));
        CenterOnParent();
        m_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { request_preview(); });
        m_invert->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { request_preview(); });
        import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { import_image(); });
        m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_result && current_source() && !m_job.valid() && !m_pending)
                EndModal(wxID_OK);
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { stop_worker(); EndModal(wxID_CANCEL); });
        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { stop_worker(); EndModal(wxID_CANCEL); });
        refresh_library();
        m_timer.Start(80);
        request_preview();
    }

    ~TextureDisplacementDialog() override
    {
        m_timer.Stop();
        stop_worker();
        // Explicitly delete the canvas while the modal parent/app still owns the GL context.
        delete m_preview;
        m_preview = nullptr;
    }

    indexed_triangle_set take_mesh() { return m_result ? std::move(*m_result) : indexed_triangle_set{}; }

private:
    wxSpinCtrlDouble* parameter(wxSizer* layout, const wxString& label, double min, double max, double initial, double step)
    {
        layout->Add(new wxStaticText(this, wxID_ANY, label), 0, wxBOTTOM, 4);
        auto* control = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                              wxSP_ARROW_KEYS, min, max, initial, step);
        control->SetDigits(2);
        layout->Add(control, 0, wxEXPAND | wxBOTTOM, 10);
        control->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { request_preview(); });
        control->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { if (m_apply) request_preview(); });
        return control;
    }

    bool current_source()
    {
        if (!m_source_is_current || m_source_is_current())
            return true;
        if (m_state)
            m_state->canceled.store(true);
        m_pending = false;
        m_result.reset();
        m_apply->Disable();
        m_status->SetLabel(_L("The selected model changed. Close this window and select it again."));
        return false;
    }

    void refresh_library(const std::string& select_path = {})
    {
        std::string selected = select_path;
        const int old_index = m_choice->GetSelection();
        if (selected.empty() && old_index >= 0 && size_t(old_index) < m_entries.size())
            selected = m_entries[size_t(old_index)].path;
        m_entries = texture_library(true);
        m_library_generation = texture_library_generation();
        m_choice->Clear();
        int selection = m_entries.empty() ? wxNOT_FOUND : 0;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            const auto& entry = m_entries[i];
            const std::string source = entry.source == TextureSource::Plugin ? entry.pack_name :
                                       entry.source == TextureSource::User ? "Imported" : "Built-in";
            m_choice->Append(from_u8(source + " / " + entry.name));
            if (entry.path == selected)
                selection = int(i);
        }
        m_choice->SetSelection(selection);
    }

    void import_image()
    {
        wxFileDialog chooser(this, _L("Import texture image"), wxEmptyString, wxEmptyString,
                             "Image files (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (chooser.ShowModal() != wxID_OK)
            return;
        std::string error;
        const auto imported = import_texture_to_library(into_u8(chooser.GetPath()), error);
        if (!imported) {
            m_status->SetLabel(from_u8(error));
            return;
        }
        refresh_library(imported->path);
        request_preview();
    }

    void request_preview()
    {
        if (!m_apply || !current_source())
            return;
        ++m_generation;
        m_result.reset();
        m_apply->Disable();
        m_pending = true;
        if (m_state)
            m_state->canceled.store(true);
        m_status->SetLabel(_L("Preparing preview…"));
    }

    void start_worker()
    {
        m_pending = false;
        const int index = m_choice->GetSelection();
        if (index < 0 || size_t(index) >= m_entries.size()) {
            m_status->SetLabel(_L("Import an image or install a local texture plugin to begin."));
            m_preview->set_mesh(m_source);
            return;
        }
        std::string error;
        const auto bytes = load_texture_image_data(m_entries[size_t(index)], error);
        if (!bytes) {
            m_status->SetLabel(from_u8(error));
            m_preview->set_mesh(m_source);
            return;
        }
        record_plugin_texture_source(m_entries[size_t(index)].path, m_entries[size_t(index)].source == TextureSource::Plugin);
        touch_user_texture(m_entries[size_t(index)].path);
        TextureDisplacementRequest request;
        request.image_data = bytes;
        request.depth_mm = float(m_depth->GetValue());
        request.tiling_scale = float(m_scale->GetValue());
        request.rotation_deg = float(m_rotation->GetValue());
        request.edge_length_mm = m_edge->GetValue();
        request.invert = m_invert->GetValue();
        request.flip_normals = m_flip;
        m_state = std::make_shared<BakeJobState>();
        const auto state = m_state;
        const auto source = m_source;
        m_job_generation = m_generation;
        m_job = std::async(std::launch::async, [source, request = std::move(request), state] {
            return build_texture_displacement_mesh(*source, request, [state](const char*, double fraction) {
                state->stage_progress.store(int(std::clamp(fraction, 0., 1.) * 100.));
                return !state->canceled.load();
            });
        });
    }

    void tick()
    {
        if (!current_source())
            return;
        if (m_library_generation != texture_library_generation()) {
            refresh_library();
            request_preview();
        }
        if (m_job.valid()) {
            if (m_job.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                m_status->SetLabel(wxString::Format(_L("Baking preview… %d%% of current stage"), m_state->stage_progress.load()));
                return;
            }
            TextureDisplacementResult result;
            try {
                result = m_job.get();
            } catch (const std::exception& ex) {
                result.error = std::string("Texture preview failed: ") + ex.what();
            } catch (...) {
                result.error = "Texture preview failed.";
            }
            if (m_job_generation == m_generation && !m_state->canceled.load() && !result.canceled) {
                if (result.success()) {
                    m_result = std::make_shared<indexed_triangle_set>(std::move(result.mesh));
                    m_preview->set_mesh(m_result);
                    m_apply->Enable();
                    m_status->SetLabel(result.safety_cap_hit || result.budget_limited ?
                        _L("Preview ready. Mesh detail was limited by the triangle budget.") : _L("Preview ready."));
                } else {
                    m_status->SetLabel(from_u8(result.error));
                }
            }
            m_state.reset();
        }
        if (m_pending) {
            try {
                start_worker();
            } catch (const std::exception& ex) {
                m_pending = false;
                m_state.reset();
                m_status->SetLabel(from_u8(std::string("Texture preview failed: ") + ex.what()));
            }
        }
    }

    void stop_worker()
    {
        m_pending = false;
        if (m_state)
            m_state->canceled.store(true);
        if (m_job.valid()) {
            try { m_job.get(); } catch (...) {}
        }
        m_state.reset();
    }

    std::shared_ptr<const indexed_triangle_set> m_source;
    std::shared_ptr<indexed_triangle_set> m_result;
    bool m_flip;
    std::function<bool()> m_source_is_current;
    TextureMeshPreview* m_preview = nullptr;
    wxChoice* m_choice = nullptr;
    wxSpinCtrlDouble *m_depth = nullptr, *m_scale = nullptr, *m_rotation = nullptr, *m_edge = nullptr;
    wxCheckBox* m_invert = nullptr;
    wxButton* m_apply = nullptr;
    wxStaticText* m_status = nullptr;
    wxTimer m_timer;
    std::vector<TextureLibraryEntry> m_entries;
    uint64_t m_library_generation = 0, m_generation = 0, m_job_generation = 0;
    bool m_pending = false;
    std::shared_ptr<BakeJobState> m_state;
    std::future<TextureDisplacementResult> m_job;
};

} // namespace

std::optional<indexed_triangle_set> show_texture_displacement_dialog(
    wxWindow* parent, indexed_triangle_set source, bool flip_normals, const std::function<bool()>& source_is_current)
{
    if (source.vertices.empty() || source.indices.empty())
        return std::nullopt;
    TextureDisplacementDialog dialog(parent, std::move(source), flip_normals, source_is_current);
    if (dialog.ShowModal() != wxID_OK || (source_is_current && !source_is_current()))
        return std::nullopt;
    return dialog.take_mesh();
}

} // namespace Slic3r::GUI
