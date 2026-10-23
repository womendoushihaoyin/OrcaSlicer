#include <GL/glew.h>
#include "EditableTextureDialog.hpp"
#include "EditableTextureUV.hpp"
#include "TextureEditHistory.hpp"
#include "TextureEditingTools.hpp"
#include "TexturePaintVisibility.hpp"
#include "TextureProjectorFrame.hpp"
#include <wx/weakref.h>
#include "TextureImageAdjustment.hpp"
#include "TextureLibrary.hpp"
#include "TextureColorBake.hpp"
#include "TextureMixedPaletteSnapshot.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "Plater.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <map>
#include "PluginWorkflowSmoke.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GLModel.hpp"
#include "GLShader.hpp"
#include "OpenGLManager.hpp"
#include "I18N.hpp"
#include "libslic3r/AABBMesh.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <wx/button.h>
#include <wx/accel.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcclient.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/glcanvas.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/slider.h>
#include <wx/statbmp.h>
#include <wx/image.h>

namespace Slic3r::GUI {
namespace {
#include "EditableTexturePreview.inc"
#include "EditableTextureImage.inc"

struct TextureMixPreview
{
    TextureColorBakeResult bake;
    std::shared_ptr<const TextureMixPlan> plan;
};

struct PreviewProgress
{
    std::atomic<bool> cancel{false};
    std::atomic<int> value{0};
};

class EditableTextureDialog final : public wxDialog
{
public:
    EditableTextureDialog(wxWindow* parent, const ModelVolume& volume, std::function<bool()> current, int instance_index)
        : wxDialog(parent, wxID_ANY, _L("Edit texture layers"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_session(volume, instance_index), m_history(m_session), m_current(std::move(current)), m_timer(this)
    {
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_controls = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(315, 580));
        m_controls->SetScrollRate(0, 15);
        auto* settings = new wxBoxSizer(wxVERTICAL);
        settings->Add(new wxStaticText(m_controls, wxID_ANY, _L("Layers (up to 8; higher slots blend over lower slots)")), 0, wxBOTTOM, 5);
        m_layers = choice(settings, {});
        auto* layer_commands = new wxBoxSizer(wxHORIZONTAL);
        button(layer_commands, _L("Remove"), [this] {
            if (m_session.remove_layer(active_slot())) { record_edit(_L("Remove layer")); changed(); refresh_layers(); }
        });
        button(layer_commands, _L("Move up"), [this] {
            if (m_session.move_layer(active_slot(), -1)) { record_edit(_L("Move layer")); changed(); refresh_layers(); }
        });
        button(layer_commands, _L("Move down"), [this] {
            if (m_session.move_layer(active_slot(), 1)) { record_edit(_L("Move layer")); changed(); refresh_layers(); }
        });
        settings->Add(layer_commands, 0, wxEXPAND | wxBOTTOM, 8);
        settings->Add(new wxStaticText(m_controls, wxID_ANY, _L("Texture library")), 0, wxBOTTOM, 5);
        m_library = choice(settings, {});
        auto* texture_commands = new wxBoxSizer(wxHORIZONTAL);
        button(texture_commands, _L("Add layer"), [this] { use_texture(false); });
        button(texture_commands, _L("Replace image"), [this] { use_texture(true); });
        button(texture_commands, _L("Import"), [this] { import_texture(); });
        settings->Add(texture_commands, 0, wxEXPAND | wxBOTTOM, 10);
        auto* image_commands = new wxBoxSizer(wxHORIZONTAL);
        button(image_commands, _L("Adjust image"), [this] { adjust_image(); });
        settings->Add(image_commands, 0, wxBOTTOM, 10);

        m_projection = choice(settings, {_L("Triplanar"), _L("Cylindrical"), _L("Spherical"),
                                         _L("UV unwrap (LSCM)"), _L("Project from view"), _L("Model UV")});
        m_blend = choice(settings, {_L("Add"), _L("Subtract"), _L("Multiply"), _L("Divide"), _L("Override")});
        m_depth = number(settings, _L("Depth (mm)"), -20., 20., .4, .05);
        m_scale = number(settings, _L("Tile size (mm)"), .01, 10000., 12.5, .5);
        m_mapping_note = new wxStaticText(m_controls, wxID_ANY, wxEmptyString);
        m_mapping_note->Wrap(290); settings->Add(m_mapping_note, 0, wxEXPAND | wxBOTTOM, 6);
        m_rotation = number(settings, _L("Rotation (degrees)"), -3600., 3600., 0., 5.);
        m_u = number(settings, _L("Offset U"), -10000., 10000., 0., .1);
        m_v = number(settings, _L("Offset V"), -10000., 10000., 0., .1);
        m_midlevel = number(settings, _L("Neutral height"), 0., 1., 0., .05);
        m_smoothing = number(settings, _L("Image smoothing"), 0., 1., 0., .05);
        m_seam_angle = number(settings, _L("UV seam angle"), 0., 180., 30., 5.);
        m_color = check(settings, _L("Paint texture colors with filaments"));
        m_color_note = new wxStaticText(m_controls, wxID_ANY, wxEmptyString);
        m_color_note->Wrap(290); settings->Add(m_color_note, 0, wxEXPAND | wxBOTTOM, 6);
        m_color_mix = check(settings, _L("Mix filaments"));
        m_mix_count = number(settings, _L("Mixed recipe limit"), 0., double(MAXIMUM_FILAMENT_NUMBER), 4., 1.);
        m_mix_count->SetDigits(0);
        m_mix_count->SetToolTip(_L("Preview proposes mixtures of physical filaments. Only recipes actually used by Bake are added to the project; flat-color images use pure filaments."));
        m_despeckle = number(settings, _L("Color despeckle passes"), 0., 10., 2., 1.);
        m_invert = check(settings, _L("Invert height"));
        m_tiling = check(settings, _L("Repeat texture"));
        m_mirror = check(settings, _L("Mirror repeated tiles"));
        m_edge = check(settings, _L("Smooth painted edges"));
        m_edge_amount = number(settings, _L("Edge smoothing"), 0., 1., .5, .05);
        m_project_visible = check(settings, _L("Project only on visible"));
        m_project_visible->SetToolTip(_L("Replaces this layer's mask with faces facing the current view and not hidden by the model. Visibility is checked at triangle centers."));
        m_project_visible->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            if (m_project_visible->GetValue() && !capture_view(true, false)) m_project_visible->SetValue(false);
        });
        auto* uv_commands = new wxBoxSizer(wxHORIZONTAL);
        button(uv_commands, _L("Capture view"), [this] { capture_view(m_project_visible->GetValue(), true); });
        button(uv_commands, _L("Edit UV islands"), [this] {
            try {
                if (current_layer() && edit_texture_uv(this, m_session, active_slot())) {
                    record_edit(_L("Edit UV islands")); changed(); load_parameters();
                }
            } catch (const std::exception& e) { report_error(e.what()); }
        });
        settings->Add(uv_commands, 0, wxTOP | wxBOTTOM, 8);
        auto* projector_commands=new wxBoxSizer(wxHORIZONTAL);
        m_frame_toggle=button(projector_commands,_L("Projection frame"),[this] { toggle_projection_frame(); });
        m_frame_apply=button(projector_commands,_L("Apply frame"),[this] { apply_projection_frame(); });
        settings->Add(projector_commands,0,wxBOTTOM,6);
        auto* flat_commands=new wxBoxSizer(wxHORIZONTAL);
        m_frame_flat=button(flat_commands,_L("Use flat projection"),[this] {
            if (auto* layer=current_layer(); layer && layer->view_project_projective) {
                layer->view_project_projective=false;
                record_edit(_L("Use flat texture projection")); changed(); load_parameters(); show_source();
            }
        });
        settings->Add(flat_commands,0,wxBOTTOM,6);
        settings->Add(new wxStaticText(m_controls,wxID_ANY,_L("Projection frame opacity")),0,wxBOTTOM,2);
        m_frame_opacity=new wxSlider(m_controls,wxID_ANY,140,20,255);
        settings->Add(m_frame_opacity,0,wxEXPAND|wxBOTTOM,6);
        m_frame_opacity->Bind(wxEVT_SLIDER,[this](wxCommandEvent&) {
            if (m_projector) m_projector->set_opacity(m_frame_opacity->GetValue());
        });
        auto* seam_commands = new wxBoxSizer(wxHORIZONTAL);
        button(seam_commands, _L("Clear seams"), [this] {
            if (auto* layer = current_layer(); layer && !layer->lscm_seam_edges.empty()) {
                layer->lscm_seam_edges.clear(); record_edit(_L("Clear UV seams")); changed(); show_source();
            }
        });
        settings->Add(seam_commands, 0, wxBOTTOM, 8);
        settings->Add(new wxStaticText(m_controls, wxID_ANY, _L("Paint active layer")), 0, wxTOP, 8);
        m_tool = choice(settings, {_L("Orbit"), _L("Sphere brush"), _L("Triangles"), _L("Smart fill"), _L("Circle brush"), _L("Mark UV seam"), _L("UV seam path")});
        m_radius = number(settings, _L("Brush radius (mm)"), .1, 100., 1., .2);
        m_fill_angle = number(settings, _L("Smart fill angle (degrees)"), 0., 90., 30., 1.);
        auto* paint_commands = new wxBoxSizer(wxHORIZONTAL);
        button(paint_commands, _L("Paint all"), [this] { paint_all(true); });
        button(paint_commands, _L("Clear mask"), [this] { paint_all(false); });
        settings->Add(paint_commands, 0, wxBOTTOM, 8);
        auto* visible_commands = new wxBoxSizer(wxHORIZONTAL);
        button(visible_commands, _L("Paint visible"), [this] { capture_view(true, false); });
        settings->Add(visible_commands, 0, wxBOTTOM, 8);
        auto* tool_help = new wxStaticText(m_controls, wxID_ANY, _L("Left drag paints; Shift + drag erases. Seam mode toggles the edge under the cursor; seam path joins two clicked vertices. Orbit rotates; scroll zooms."));
        tool_help->Wrap(290); settings->Add(tool_help, 0, wxEXPAND | wxBOTTOM, 8);
        m_result_note = new wxStaticText(m_controls, wxID_ANY, wxEmptyString);
        settings->Add(m_result_note, 0, wxEXPAND | wxBOTTOM, 6);
        m_displace_border = check(settings, _L("Displace up to the border"));
        m_displace_border->SetToolTip(_L("Move the painted area's outer ring with the relief. Turn off to keep that ring flat."));
        m_smooth_result = check(settings, _L("Smooth result"));
        m_smooth_result->SetToolTip(_L("Smooth displaced geometry after applying all layers. Image smoothing above changes the image before displacement."));
        m_smooth_strength = number(settings, _L("Geometry smoothing strength (%)"), 1., 100., 30., 1.);
        m_smooth_passes = number(settings, _L("Geometry smoothing passes"), 1., 10., 2., 1.);
        m_smooth_passes->SetDigits(0);
        m_smooth_skip_border = check(settings, _L("Ignore outer ring when smoothing"));
        m_smooth_skip_border->SetToolTip(_L("Keep the painted area's outer ring out of smoothing so the relief does not flatten against unpainted neighbors."));
        m_pipeline = check(settings, _L("Refine mesh during bake"));
        m_resolution = number(settings, _L("Bake resolution (mm; 0 = automatic)"), 0., 5., 0., .02);
        m_budget = number(settings, _L("Triangle budget (thousands; -1 = automatic, 0 = no simplification)"), -1., 5000., -1., 50.);
        m_status = new wxStaticText(m_controls, wxID_ANY, wxEmptyString);
        m_status->Wrap(290); settings->Add(m_status, 0, wxEXPAND | wxTOP, 8);
        m_controls->SetSizer(settings);
        row->Add(m_controls, 0, wxALL | wxEXPAND, 10);
        m_preview = new EditableTexturePreview(this, source_view());
        m_existing_mmu = volume.mmu_segmentation_facets.get_data();
        int base_filament = volume.config.has("extruder") ? volume.config.extruder() : 0;
        if (base_filament <= 0 && volume.get_object() && volume.get_object()->config.has("extruder"))
            base_filament = volume.get_object()->config.extruder();
        m_preview->set_default_filament(std::max(1, base_filament));
        m_preview->configure_paint(m_session.source, m_session.volume_to_world,
            [this](const TriangleSelector::TriangleSplittingData& mask) {
                if (active_slot() >= 0) { m_session.masks[size_t(active_slot())] = mask; changed(false); }
            }, [this](bool begin) {
                if (begin) m_history.end_group();
                else record_edit(_L("Paint texture mask"));
            });
        row->Add(m_preview, 1, wxALL | wxEXPAND, 10);
        root->Add(row, 1, wxEXPAND);
        auto* explanation = new wxStaticText(this, wxID_ANY,
            _L("Save edits keeps layers, images, masks and UV editable; it does not change the printed surface. Bake into mesh before slicing the relief or colors. Bake consumes its painted masks; layer parameters remain available. Cancel changes nothing."));
        explanation->Wrap(930); root->Add(explanation, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        auto* history_commands = new wxBoxSizer(wxHORIZONTAL);
        m_undo = new wxButton(this, wxID_UNDO, _L("Undo edit"));
        m_redo = new wxButton(this, wxID_REDO, _L("Redo edit"));
        history_commands->Add(m_undo, 0, wxRIGHT, 6); history_commands->Add(m_redo);
        root->Add(history_commands, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        auto* commands = new wxBoxSizer(wxHORIZONTAL);
        m_build = new wxButton(this, wxID_ANY, _L("Preview displacement"));
        commands->Add(m_build, 0, wxRIGHT, 6);
        auto* source_button = new wxButton(this, wxID_ANY, _L("Show source / paint"));
        commands->Add(source_button, 0, wxRIGHT, 6);
        commands->AddStretchSpacer();
        auto* cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        m_save = new wxButton(this, wxID_ANY, _L("Save editable layers"));
        m_bake = new wxButton(this, wxID_ANY, _L("Bake into mesh")); m_bake->Disable();
        commands->Add(cancel, 0, wxRIGHT, 6); commands->Add(m_save, 0, wxRIGHT, 6); commands->Add(m_bake);
        root->Add(commands, 0, wxALL | wxEXPAND, 10);
        SetSizerAndFit(root); SetSize(wxSize(1050, 760)); SetMinSize(wxSize(850, 650)); CentreOnParent();
        m_layers->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            if (m_layers->GetSelection() >= 0) {
                m_history.end_group();
                m_session.options.active_slot = m_session.layers[size_t(m_layers->GetSelection())].slot;
                load_parameters(); show_source();
            }
        });
        m_tool->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { update_tool_controls(); show_source(); });
        m_undo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { navigate_history(false); });
        m_redo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { navigate_history(true); });
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { navigate_history(false); }, wxID_UNDO);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { navigate_history(true); }, wxID_REDO);
        wxAcceleratorEntry accelerators[3];
        accelerators[0].Set(wxACCEL_CMD, 'Z', wxID_UNDO);
        accelerators[1].Set(wxACCEL_CMD | wxACCEL_SHIFT, 'Z', wxID_REDO);
        accelerators[2].Set(wxACCEL_CMD, 'Y', wxID_REDO);
        SetAcceleratorTable(wxAcceleratorTable(3, accelerators));
        m_build->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_preview(); });
        source_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_source(); });
        m_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { finish(false); });
        m_bake->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { finish(true); });
        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_close(wxID_CANCEL); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { request_close(wxID_CANCEL); });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
        for (auto* control : {m_projection, m_blend})
            control->Bind(wxEVT_CHOICE, [this, control](wxCommandEvent&) { store_parameters(control); });
        for (auto* control : {m_invert, m_tiling, m_mirror, m_edge, m_color, m_color_mix, m_pipeline,
                              m_displace_border, m_smooth_result, m_smooth_skip_border})
            control->Bind(wxEVT_CHECKBOX, [this, control](wxCommandEvent&) { store_parameters(control); });
        m_tool->SetSelection(0);
        refresh_library(); refresh_layers(); update_tool_controls(); update_history_controls();
        m_timer.Start(100);
    }
    ~EditableTextureDialog() override
    {
        m_timer.Stop();
        if (m_projector) m_projector->Destroy();
        if (m_progress) m_progress->cancel = true;
        if (m_future.valid()) { try { m_future.get(); } catch (...) {} }
    }
    EditableTextureResult result()
    {
        if (m_commit_bake && m_completed && m_completed->plan) {
            auto commit = std::make_shared<TextureMixCommit>(resolve_texture_mix_commit(*m_completed->plan,std::move(m_completed->bake)));
            if (!commit->created_stable_ids.empty())
                return {std::move(m_session),std::nullopt,{},std::move(commit),m_palette_before};
            return {std::move(m_session),std::move(commit->bake.mesh),std::move(commit->bake.texture_filaments),{}, {}};
        }
        return {std::move(m_session),
                m_commit_bake && m_baked ? std::optional<indexed_triangle_set>(std::move(*m_baked)) : std::nullopt,
                m_commit_bake ? std::move(m_baked_filaments) : std::vector<uint8_t>{}, {}, {}};
    }
private:
    std::shared_ptr<const indexed_triangle_set> source_view() const
    { return std::shared_ptr<const indexed_triangle_set>(m_session.source, &m_session.source->its); }
    int active_slot() const { return current_layer() ? m_session.options.active_slot : -1; }
    TextureDisplacementLayer* current_layer() { return m_session.layer(m_session.options.active_slot); }
    const TextureDisplacementLayer* current_layer() const { return m_session.layer(m_session.options.active_slot); }
    void report_error(const std::string& message)
    { wxMessageBox(from_u8(message), _L("Texture"), wxOK | wxICON_ERROR, this); }
    wxChoice* choice(wxSizer* settings, std::initializer_list<wxString> names)
    {
        wxArrayString choices;
        for (const auto& name : names) choices.Add(name);
        auto* result = new wxChoice(m_controls, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
        result->SetMinSize(wxSize(290, -1)); settings->Add(result, 0, wxEXPAND | wxBOTTOM, 6); return result;
    }
    wxCheckBox* check(wxSizer* settings, const wxString& label)
    {
        auto* result = new wxCheckBox(m_controls, wxID_ANY, label);
        settings->Add(result, 0, wxBOTTOM, 5); return result;
    }
    wxSpinCtrlDouble* number(wxSizer* settings, const wxString& label, double lo, double hi, double value, double increment)
    {
        settings->Add(new wxStaticText(m_controls, wxID_ANY, label), 0, wxBOTTOM, 2);
        auto* result = new wxSpinCtrlDouble(m_controls, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                          wxSP_ARROW_KEYS, lo, hi, value, increment);
        result->SetDigits(3); settings->Add(result, 0, wxEXPAND | wxBOTTOM, 6);
        result->Bind(wxEVT_SPINCTRLDOUBLE, [this, result](wxSpinDoubleEvent&) { store_parameters(result); });
        result->Bind(wxEVT_TEXT, [this, result](wxCommandEvent&) { store_parameters(result); });
        result->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { m_history.end_group(); event.Skip(); });
        return result;
    }
    wxButton* button(wxSizer* row, const wxString& label, std::function<void()> action)
    {
        auto* control = new wxButton(m_controls, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        control->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent&) {
            try { action(); } catch (const std::exception& e) { report_error(e.what()); }
        });
        row->Add(control, 0, wxRIGHT, 4);
        return control;
    }
    void refresh_library()
    {
        m_entries = texture_library(true); m_library->Clear();
        for (const auto& entry : m_entries) {
            const std::string source = entry.source == TextureSource::Plugin ? entry.pack_name :
                                       entry.source == TextureSource::User ? "User" : "Built-in";
            m_library->Append(from_u8(source + " / " + entry.name));
        }
        if (!m_entries.empty()) m_library->SetSelection(0);
        m_library_generation = texture_library_generation();
    }
    void use_texture(bool replace)
    {
        const int selected = m_library->GetSelection();
        if (selected < 0 || size_t(selected) >= m_entries.size()) return;
        std::string error;
        const auto entry = m_entries[size_t(selected)];
        auto image = load_texture_image_data(entry, error); // revalidates/pins enabled plugin provenance
        if (!image) throw std::runtime_error(error);
        record_plugin_texture_source(entry.path, entry.source == TextureSource::Plugin);
        if (replace && current_layer()) {
            auto* layer = current_layer();
            layer->name = entry.name; layer->path = entry.path; layer->path_in_3mf.clear(); layer->image_data = std::move(image);
        } else {
            m_session.add_layer(entry.name, entry.path, std::move(image));
        }
        touch_user_texture(entry.path); record_edit(replace ? _L("Replace texture image") : _L("Add layer")); changed(); refresh_layers();
    }
    void adjust_image()
    {
        auto* layer = current_layer();
        if (!layer) return;
        m_image_adjustment_active = true;
        struct ActiveGuard { bool& active; ~ActiveGuard() { active = false; } } guard{m_image_adjustment_active};
        EditableTextureImageDialog dialog(this,*layer,[this] {
            changed(false);
            m_image_preview_pending = std::any_of(m_session.masks.begin(),m_session.masks.end(),[](const auto& mask) {
                return TriangleSelector::has_facets(mask,EnforcerBlockerType::ENFORCER);
            });
        });
        if (dialog.ShowModal() == wxID_OK) {
            record_edit(_L("Adjust texture image")); refresh_library();
        }
        load_parameters();
    }
    void import_texture()
    {
        wxFileDialog dialog(this, _L("Import texture"), wxEmptyString, wxEmptyString,
                            "Images|*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        std::string error;
        auto entry = import_texture_to_library(into_u8(dialog.GetPath()), error);
        if (!entry) throw std::runtime_error(error);
        auto image = load_texture_image_data(*entry, error);
        if (!image) throw std::runtime_error(error);
        m_session.add_layer(entry->name, entry->path, std::move(image));
        record_edit(_L("Import texture layer")); changed(); refresh_library(); refresh_layers();
    }
    void refresh_layers()
    {
        m_loading = true; m_layers->Clear();
        for (size_t i = 0; i < m_session.layers.size(); ++i) {
            const auto& layer = m_session.layers[i];
            m_layers->Append(from_u8(std::to_string(layer.slot + 1) + ": " + layer.name));
            if (layer.slot == m_session.options.active_slot) m_layers->SetSelection(int(i));
        }
        if (m_layers->GetSelection() == wxNOT_FOUND && !m_session.layers.empty()) {
            m_layers->SetSelection(0); m_session.options.active_slot = m_session.layers.front().slot;
        }
        m_loading = false; load_parameters(); show_source();
    }
    void load_parameters()
    {
        m_loading = true;
        if (const auto* layer = current_layer()) {
            m_projection->SetSelection(int(layer->projection_method)); m_blend->SetSelection(int(layer->blend_mode));
            m_depth->SetValue(layer->depth_mm); m_scale->SetValue(layer->tiling_scale); m_rotation->SetValue(layer->rotation_deg);
            m_u->SetValue(layer->offset.x()); m_v->SetValue(layer->offset.y()); m_midlevel->SetValue(layer->midlevel);
            m_smoothing->SetValue(layer->smoothing); m_seam_angle->SetValue(layer->lscm_seam_angle_deg);
            m_invert->SetValue(layer->invert); m_tiling->SetValue(layer->tile_enabled);
            m_mirror->SetValue(layer->tile_method == TextureTileMethod::MirroredRepeat);
            m_edge->SetValue(layer->edge_smoothing); m_edge_amount->SetValue(layer->edge_smoothing_amount);
            m_color->SetValue(layer->color_enabled);
            if (decode_height_texture(*layer).empty())
                m_status->SetLabel(_L("This layer has no usable embedded image. Replace image to restore it; its paint and parameters are preserved."));
            else
                m_status->SetLabel(layer->color_enabled ? _L("Texture colors are matched to existing filament slots. Outside this layer's mask, existing color painting is preserved.") : wxString());
        }
        m_pipeline->SetValue(m_session.options.pipeline_v2);
        m_color_mix->SetValue(m_session.options.color_mix_enabled);
        m_mix_count->SetValue(m_session.options.color_mix_count);
        m_despeckle->SetValue(m_session.options.color_despeckle);
        m_resolution->SetValue(m_session.options.v2_refine_mm);
        m_budget->SetValue(m_session.options.v2_max_triangles_k);
        m_displace_border->SetValue(m_session.options.displace_border);
        m_smooth_result->SetValue(m_session.options.smooth_enabled);
        m_smooth_strength->SetValue(100. * m_session.options.smooth_strength);
        m_smooth_passes->SetValue(m_session.options.smooth_iterations);
        m_smooth_skip_border->SetValue(m_session.options.smooth_skip_border);
        m_loading = false;
        update_layer_controls();
    }
    void update_layer_controls()
    {
        const auto* layer = current_layer();
        const bool model_uv = layer && layer->projection_method == TextureProjectionMethod::ModelUV;
        const bool view_projected=layer && layer->projection_method==TextureProjectionMethod::ViewProjected;
        const bool framed=view_projected && layer->view_project_projective;
        m_project_visible->Enable(view_projected);
        m_frame_toggle->Enable(view_projected); m_frame_apply->Enable(view_projected);
        m_frame_flat->Enable(framed); m_frame_opacity->Enable(view_projected);
        if (m_projector) {
            if (!view_projected) m_projector->Hide();
            else if (m_projector->IsShown()) update_projection_frame();
        }
        m_scale->Enable(layer && !model_uv && !framed);
        m_rotation->Enable(layer && !framed); m_u->Enable(layer && !framed); m_v->Enable(layer && !framed);
        m_mapping_note->SetLabel(model_uv ? _L("Model UV uses the imported normalized layout; tile size does not affect this projection.") : framed ? _L("The captured frame controls texture placement. Use flat projection to edit tile size, rotation or offsets.") : wxString());
        m_mix_count->Enable(m_session.options.color_mix_enabled);
        const bool has_color = layer && decode_height_texture(*layer).has_color();
        m_color->Enable(has_color);
        m_color_note->SetLabel(layer && !has_color ? _L("This image has no RGB colors to apply. Import or replace it with a color image; any stored color setting is preserved.") : wxString());
        const bool classic_bake = !m_session.options.pipeline_v2;
        m_displace_border->Enable(classic_bake);
        m_smooth_result->Enable(classic_bake);
        m_smooth_strength->Enable(classic_bake && m_session.options.smooth_enabled);
        m_smooth_passes->Enable(classic_bake && m_session.options.smooth_enabled);
        m_smooth_skip_border->Enable(classic_bake && m_session.options.smooth_enabled);
        m_result_note->SetLabel(classic_bake ? wxString() :
            _L("Border displacement and geometry smoothing apply to classic baking. Turn off Refine mesh during bake to use them. Saved settings are kept."));
        m_result_note->Wrap(290);
        m_resolution->Enable(m_session.options.pipeline_v2);
        m_budget->Enable(m_session.options.pipeline_v2);
        m_mapping_note->Wrap(290); m_color_note->Wrap(290);
        m_controls->Layout(); m_controls->FitInside();
    }
    void update_tool_controls()
    {
        const int tool = m_tool->GetSelection();
        m_radius->Enable(tool == 1 || tool == 4);
        m_fill_angle->Enable(tool == 3);
    }
    void store_parameters(wxWindow* control)
    {
        if (m_loading || !m_preview) return;
        if (control == m_radius || control == m_fill_angle) { show_source(); return; }
        // Touch only the edited field. A saved value outside a widget's display
        // range, or automatic (-1) budget, must not change on an unrelated edit.
        if (auto* layer = current_layer()) {
            if (control == m_projection) layer->projection_method = TextureProjectionMethod(m_projection->GetSelection());
            if (control == m_blend) layer->blend_mode = TextureBlendMode(m_blend->GetSelection());
            if (control == m_depth) layer->depth_mm = float(m_depth->GetValue());
            if (control == m_scale && m_scale->IsEnabled()) layer->tiling_scale = float(m_scale->GetValue());
            if (control == m_rotation) layer->rotation_deg = float(m_rotation->GetValue());
            if (control == m_u) layer->offset.x() = float(m_u->GetValue());
            if (control == m_v) layer->offset.y() = float(m_v->GetValue());
            if (control == m_midlevel) layer->midlevel = float(m_midlevel->GetValue());
            if (control == m_smoothing) layer->smoothing = float(m_smoothing->GetValue());
            if (control == m_seam_angle) layer->lscm_seam_angle_deg = float(m_seam_angle->GetValue());
            if (control == m_invert) layer->invert = m_invert->GetValue();
            if (control == m_tiling) layer->tile_enabled = m_tiling->GetValue();
            if (control == m_mirror) layer->tile_method = m_mirror->GetValue() ? TextureTileMethod::MirroredRepeat : TextureTileMethod::Repeat;
            if (control == m_color && m_color->IsEnabled()) layer->color_enabled = m_color->GetValue();
            if (control == m_edge) layer->edge_smoothing = m_edge->GetValue();
            if (control == m_edge_amount) layer->edge_smoothing_amount = float(m_edge_amount->GetValue());
        }
        if (control == m_pipeline) m_session.options.pipeline_v2 = m_pipeline->GetValue();
        if (control == m_color_mix) m_session.options.color_mix_enabled = m_color_mix->GetValue();
        if (control == m_mix_count && m_mix_count->IsEnabled()) m_session.options.color_mix_count = int(m_mix_count->GetValue());
        if (control == m_despeckle) m_session.options.color_despeckle = int(m_despeckle->GetValue());
        if (control == m_resolution && m_resolution->IsEnabled()) m_session.options.v2_refine_mm = float(m_resolution->GetValue());
        if (control == m_budget && m_budget->IsEnabled()) m_session.options.v2_max_triangles_k = int(m_budget->GetValue());
        if (control == m_displace_border && m_displace_border->IsEnabled()) m_session.options.displace_border = m_displace_border->GetValue();
        if (control == m_smooth_result && m_smooth_result->IsEnabled()) m_session.options.smooth_enabled = m_smooth_result->GetValue();
        if (control == m_smooth_strength && m_smooth_strength->IsEnabled())
            m_session.options.smooth_strength = float(m_smooth_strength->GetValue() / 100.);
        if (control == m_smooth_passes && m_smooth_passes->IsEnabled())
            m_session.options.smooth_iterations = int(m_smooth_passes->GetValue());
        if (control == m_smooth_skip_border && m_smooth_skip_border->IsEnabled())
            m_session.options.smooth_skip_border = m_smooth_skip_border->GetValue();
        update_layer_controls();
        record_edit(_L("Change texture settings"), dynamic_cast<wxSpinCtrlDouble*>(control) ? reinterpret_cast<uintptr_t>(control) : 0);
        changed(); show_source();
    }
    void update_projection_frame()
    {
        if (!m_projector || !current_layer()) return;
        const auto& layer=*current_layer();
        if (m_projector_image==layer.image_data.get() && m_projector_smoothing==layer.smoothing) return;
        const auto image=decode_height_texture(layer);
        m_projector->set_texture(image.pixels,image.width,image.height);
        m_projector_image=layer.image_data.get(); m_projector_smoothing=layer.smoothing;
    }
    void toggle_projection_frame()
    {
        const auto* layer=current_layer();
        if (!layer || layer->projection_method!=TextureProjectionMethod::ViewProjected) return;
        if (m_projector && m_projector->IsShown()) { m_projector->Hide(); return; }
        if (!m_projector) {
            m_projector=new TextureProjectorFrame(this);
            m_projector->SetSize(m_preview->default_projection_frame());
            m_projector_image=nullptr; m_projector_smoothing=-1.f;
        }
        update_projection_frame();
        m_projector->set_opacity(m_frame_opacity->GetValue());
        m_projector->Show();
    }
    void apply_projection_frame()
    {
        auto* layer=current_layer();
        if (!layer || !m_projector || !m_projector->IsShown())
            throw std::runtime_error("Open the projection frame and position it over the model first");
        m_preview->finish_stroke();
        TextureDisplacementLayer candidate=*layer;
        m_preview->project_frame(candidate,m_projector->client_rect_on_screen());
        auto mask=m_preview->visible_mask(&candidate.view_project_matrix);
        // Build both projection and replacement mask first: an empty frame must
        // not erase paint or leave half of a failed capture in the recipe.
        *layer=std::move(candidate); m_session.masks[size_t(active_slot())]=std::move(mask);
        record_edit(_L("Apply texture projection frame")); changed(); load_parameters(); show_source();
        m_projector->Hide();
    }
    bool capture_view(bool visible_only, bool capture_projection)
    {
        auto* layer = current_layer();
        if (!layer) return false;
        try {
            m_preview->finish_stroke();
            std::optional<TriangleSelector::TriangleSplittingData> mask;
            if (visible_only) {
                wxBusyCursor wait;
                mask = m_preview->visible_mask(); // Compute completely before replacing axes or paint.
            }
            if (capture_projection) m_preview->project_from_view(*layer);
            if (mask) m_session.masks[size_t(active_slot())] = std::move(*mask);
            record_edit(capture_projection ? _L("Capture texture view") : _L("Paint visible faces"));
            changed(); load_parameters(); show_source();
            return true;
        } catch (const std::exception& e) { report_error(e.what()); return false; }
    }
    void paint_all(bool value)
    {
        if (active_slot() < 0) return;
        TriangleSelector selector(*m_session.source);
        if (value)
            for (size_t i = 0; i < m_session.source->its.indices.size(); ++i)
                selector.set_facet(int(i), EnforcerBlockerType::ENFORCER);
        m_session.masks[size_t(active_slot())] = selector.serialize();
        record_edit(value ? _L("Paint whole model") : _L("Clear texture mask")); changed(); show_source();
    }
    void show_source()
    {
        m_preview->set_mesh(source_view());
        if (active_slot() >= 0) m_preview->set_mask(m_session.masks[size_t(active_slot())]);
        m_preview->set_tool(m_tool->GetSelection(), float(m_radius->GetValue()), active_slot() >= 0, float(m_fill_angle->GetValue()));
        m_preview->set_seams(current_layer() ? current_layer()->lscm_seam_edges : std::vector<std::pair<int,int>>{},
            active_slot(), [this](const std::vector<std::pair<int,int>>& seams) {
                if (auto* layer = current_layer()) {
                    layer->lscm_seam_edges = seams; layer->projection_method = TextureProjectionMethod::LSCM;
                    record_edit(_L("Mark UV seam")); changed(false); load_parameters();
                }
            });
    }
    void update_history_controls()
    {
        if (!m_undo || !m_redo) return;
        m_undo->Enable(m_history.can_undo()); m_redo->Enable(m_history.can_redo());
        m_undo->SetToolTip(m_history.can_undo() ? from_u8(m_history.undo_label()) : _L("No edit to undo"));
        m_redo->SetToolTip(m_history.can_redo() ? from_u8(m_history.redo_label()) : _L("No edit to redo"));
    }
    void record_edit(const wxString& label, uintptr_t group = 0)
    {
        m_history.record(m_session, into_u8(label), group);
        update_history_controls();
    }
    void navigate_history(bool redo)
    {
        if (m_close_code != 0 || m_image_adjustment_active) return;
        m_preview->finish_stroke();
        if (redo ? m_history.redo(m_session) : m_history.undo(m_session)) {
            changed(); refresh_layers(); update_history_controls();
        }
    }
    void changed(bool reset_view = true)
    {
        ++m_generation; m_baked.reset(); m_completed.reset(); m_baked_filaments.clear(); m_bake->Disable();
        if (m_progress) m_progress->cancel = true;
        if (reset_view && m_preview) m_preview->set_mesh(source_view());
    }
    TextureMixPolicy current_mix_policy() const
    {
        const auto& bundle=*wxGetApp().preset_bundle;
        const auto read=[&](const char* key) {
            for (const auto* config : {&bundle.project_config,&bundle.prints.get_edited_preset().config}) {
                if (const auto* value=config->option<ConfigOptionBool>(key)) return value->value;
                if (const auto* value=config->option<ConfigOptionInt>(key)) return value->value!=0;
            }
            return false;
        };
        return {read("mixed_filament_gradient_mode")?1:0,read("mixed_filament_advanced_dithering")};
    }
    void capture_palette()
    {
        auto* plater = wxGetApp().plater();
        if (!plater || !wxGetApp().preset_bundle) throw std::runtime_error("No active filament palette");
        m_filament_presets = wxGetApp().preset_bundle->filament_presets;
        m_mixed_palette = wxGetApp().preset_bundle->mixed_filaments.mixed_filaments();
        m_palette_hex = plater->get_extruder_colors_from_plater_config();
        m_palette_physical = wxGetApp().filaments_cnt();
        if (m_palette_physical < 1 || size_t(m_palette_physical) > m_palette_hex.size())
            throw std::runtime_error("The physical filament palette is incomplete");
        const std::vector<std::string> physical(m_palette_hex.begin(),m_palette_hex.begin()+m_palette_physical);
        const auto& bundle=*wxGetApp().preset_bundle;
        m_palette_before=capture_texture_mixed_palette(bundle.mixed_filaments,bundle.project_config,
            bundle.prints.get_edited_preset().config,m_filament_presets,physical);
        m_mix_context=build_mixed_filament_display_context(physical);
        m_mix_policy=current_mix_policy();
        m_palette.clear();
        for (size_t index = 0; index < m_palette_hex.size(); ++index) {
            ColorRGBA color;
            if (!decode_color(m_palette_hex[index], color))
                throw std::runtime_error("A filament has an invalid color; correct it before previewing texture colors");
            m_palette.push_back({int(index + 1), Vec3f(color.r(), color.g(), color.b()), int(index) >= m_palette_physical});
        }
    }
    bool palette_is_current() const
    {
        auto* plater = wxGetApp().plater();
        return plater && wxGetApp().preset_bundle && m_palette_hex == plater->get_extruder_colors_from_plater_config() &&
               m_palette_physical == wxGetApp().filaments_cnt() &&
               m_filament_presets == wxGetApp().preset_bundle->filament_presets &&
               m_mixed_palette == wxGetApp().preset_bundle->mixed_filaments.mixed_filaments() &&
               mix_inputs_are_current();
    }
    bool mix_inputs_are_current() const
    {
        if (!m_palette_before) return false;
        const auto& bundle=*wxGetApp().preset_bundle;
        const auto current=capture_texture_mixed_palette(bundle.mixed_filaments,bundle.project_config,
            bundle.prints.get_edited_preset().config,m_filament_presets,m_palette_before->physical_colors);
        const auto context=build_mixed_filament_display_context(m_palette_before->physical_colors);
        const auto policy=current_mix_policy();
        const auto& a=context.preview_settings;
        const auto& b=m_mix_context.preview_settings;
        return current->project_has_definitions==m_palette_before->project_has_definitions &&
            current->print_has_definitions==m_palette_before->print_has_definitions &&
            current->project_definitions==m_palette_before->project_definitions && current->print_definitions==m_palette_before->print_definitions &&
            context.nozzle_diameters==m_mix_context.nozzle_diameters && context.component_bias_enabled==m_mix_context.component_bias_enabled &&
            a.nominal_layer_height==b.nominal_layer_height && a.mixed_lower_bound==b.mixed_lower_bound && a.mixed_upper_bound==b.mixed_upper_bound &&
            a.preferred_a_height==b.preferred_a_height && a.preferred_b_height==b.preferred_b_height && a.local_z_mode==b.local_z_mode &&
            a.local_z_direct_multicolor==b.local_z_direct_multicolor && a.wall_loops==b.wall_loops &&
            policy.gradient_mode==m_mix_policy.gradient_mode && policy.advanced_dithering==m_mix_policy.advanced_dithering;
    }
    void start_preview()
    {
        if (m_future.valid() || !current_source()) return;
        m_history.end_group();
        try { m_session.validate(); } catch (const std::exception& e) { report_error(e.what()); return; }
        const bool painted = std::any_of(m_session.masks.begin(), m_session.masks.end(), [](const auto& mask) {
            return TriangleSelector::has_facets(mask, EnforcerBlockerType::ENFORCER);
        });
        if (!painted) { report_error("Paint a mask on a texture layer before previewing"); return; }
        try { capture_palette(); } catch (const std::exception& e) { report_error(e.what()); return; }
        auto progress = std::make_shared<PreviewProgress>(); m_progress = progress;
        m_job_generation = m_generation;
        const TextureEditSession input = m_session;
        m_build->Disable(); m_bake->Disable();
        m_status->SetLabel(_L("Building displacement preview..."));
        const auto palette = m_palette;
        const auto existing = m_existing_mmu;
        const auto source=m_palette_before;
        const auto context=m_mix_context;
        const auto policy=m_mix_policy;
        m_future = std::async(std::launch::async, [input, progress, palette, existing, source, context, policy]() {
            auto output=std::make_shared<TextureMixPreview>();
            const bool mixing=input.options.color_mix_enabled && std::any_of(input.layers.begin(),input.layers.end(),
                [](const auto& layer){return layer.color_enabled;});
            if (mixing) {
                auto plan=std::make_shared<TextureMixPlan>(plan_texture_mix_palette(input.layers,palette,source->manager,context,policy,
                    input.options.color_mix_count,[progress](int percentage){progress->value=percentage/10;return !progress->cancel.load();}));
                if (plan->canceled) return output;
                output->plan=std::move(plan);
            }
            output->bake=bake_texture_session(input,output->plan?output->plan->palette:palette,existing,
                [progress](int percentage){progress->value=10+percentage*90/100;return !progress->cancel.load();});
            return output;
        });
    }
    bool current_source()
    {
        if (!m_current || m_current()) return true;
        m_save->Disable(); m_bake->Disable(); m_build->Disable();
        m_status->SetLabel(_L("The selected part changed. Close and reopen the editor; no edits were applied."));
        return false;
    }
    void tick()
    {
        if (texture_library_generation() != m_library_generation) refresh_library();
        if (!m_future.valid()) {
            if (m_image_preview_pending && m_close_code == 0) { m_image_preview_pending = false; start_preview(); }
            return;
        }
        if (m_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            m_status->SetLabel(wxString::Format(_L("Building preview: %d%%"), m_progress->value.load())); return;
        }
        try {
            auto result = m_future.get();
            if (!m_progress->cancel && m_job_generation == m_generation && !result->bake.mesh.indices.empty() && current_source() && palette_is_current()) {
                m_completed=result;
                m_baked = std::shared_ptr<indexed_triangle_set>(result, &result->bake.mesh);
                m_baked_filaments = result->bake.texture_filaments;
                m_preview->set_mesh(m_baked, result->bake.preview_filaments, result->plan?result->plan->preview_palette:m_palette);
                m_preview->set_tool(0, 1.f, false);
                m_bake->Enable();
                m_status->SetLabel(wxString::Format(_L("Preview ready: %zu triangles. Save edits preserves the painted masks; Bake consumes them."), m_baked->indices.size()));
            } else if (m_close_code == 0) m_status->SetLabel(_L("Preview canceled or superseded. The editable layers are unchanged."));
        } catch (const std::exception& e) { if (m_close_code == 0) report_error(e.what()); }
        m_build->Enable(current_source());
        if (m_close_code != 0) EndModal(m_close_code);
        else if (m_image_preview_pending) { m_image_preview_pending = false; start_preview(); }
    }
    void finish(bool bake)
    {
        if (!current_source() || (bake && (!m_baked || m_future.valid()))) return;
        if (bake && !palette_is_current()) {
            m_bake->Disable();
            report_error("The filament palette changed. Preview again before baking so colors cannot use stale filament IDs.");
            return;
        }
        try { m_session.validate(); } catch (const std::exception& e) { report_error(e.what()); return; }
        m_commit_bake = bake; request_close(wxID_OK);
    }
    void request_close(int code)
    {
        m_close_code = code; m_image_preview_pending = false;
        if (m_progress) m_progress->cancel = true;
        if (!m_future.valid()) EndModal(code);
        else { m_save->Disable(); m_bake->Disable(); m_build->Disable(); m_status->SetLabel(_L("Canceling preview...")); }
    }

    bool m_image_preview_pending = false, m_image_adjustment_active = false;
    TextureEditSession m_session;
    TextureEditHistory m_history;
    std::function<bool()> m_current;
    wxTimer m_timer;
    wxScrolledWindow* m_controls = nullptr;
    EditableTexturePreview* m_preview = nullptr;
    wxChoice *m_layers = nullptr, *m_library = nullptr, *m_projection = nullptr, *m_blend = nullptr, *m_tool = nullptr;
    wxSpinCtrlDouble *m_depth = nullptr, *m_scale = nullptr, *m_rotation = nullptr, *m_u = nullptr, *m_v = nullptr,
                     *m_midlevel = nullptr, *m_smoothing = nullptr, *m_seam_angle = nullptr, *m_radius = nullptr, *m_fill_angle = nullptr,
                     *m_edge_amount = nullptr, *m_resolution = nullptr, *m_budget = nullptr, *m_despeckle = nullptr,
                     *m_smooth_strength = nullptr, *m_smooth_passes = nullptr, *m_mix_count = nullptr;
    wxCheckBox *m_invert = nullptr, *m_tiling = nullptr, *m_mirror = nullptr, *m_edge = nullptr, *m_color = nullptr, *m_color_mix = nullptr, *m_pipeline = nullptr;
    wxCheckBox *m_project_visible = nullptr;
    wxButton *m_frame_toggle=nullptr, *m_frame_apply=nullptr, *m_frame_flat=nullptr;
    wxSlider* m_frame_opacity=nullptr;
    wxWeakRef<TextureProjectorFrame> m_projector;
    const std::vector<unsigned char>* m_projector_image=nullptr;
    float m_projector_smoothing=-1.f;
    wxCheckBox *m_displace_border = nullptr, *m_smooth_result = nullptr, *m_smooth_skip_border = nullptr;
    wxButton *m_build = nullptr, *m_save = nullptr, *m_bake = nullptr, *m_undo = nullptr, *m_redo = nullptr;
    wxStaticText *m_status = nullptr, *m_mapping_note = nullptr, *m_color_note = nullptr, *m_result_note = nullptr;
    std::vector<TextureLibraryEntry> m_entries;
    uint64_t m_library_generation = 0, m_generation = 0, m_job_generation = 0;
    bool m_loading = true, m_commit_bake = false;
    int m_close_code = 0;
    std::shared_ptr<PreviewProgress> m_progress;
    std::future<std::shared_ptr<TextureMixPreview>> m_future;
    std::shared_ptr<TextureMixPreview> m_completed;
    std::shared_ptr<const TextureMixedPaletteSnapshot> m_palette_before;
    MixedFilamentDisplayContext m_mix_context;
    TextureMixPolicy m_mix_policy;
    TriangleSelector::TriangleSplittingData m_existing_mmu;
    TexturePalette m_palette;
    std::vector<std::string> m_palette_hex, m_filament_presets;
    std::vector<MixedFilament> m_mixed_palette;
    int m_palette_physical = 0;
    std::vector<uint8_t> m_baked_filaments;
    std::shared_ptr<indexed_triangle_set> m_baked;
};
} // namespace

std::optional<EditableTextureResult> show_editable_texture_dialog(
    wxWindow* parent, const ModelVolume& volume, const std::function<bool()>& source_is_current, int instance_index)
{
    EditableTextureDialog dialog(parent, volume, source_is_current, instance_index);
    if (dialog.ShowModal() != wxID_OK) return std::nullopt;
    return dialog.result();
}
} // namespace Slic3r::GUI
