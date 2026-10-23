// The source image is immutable while this modal editor is open. Temporary
// adjustments affect the actual decode/bake pipeline; Save as copy resolves them.
class EditableTextureImageDialog final : public wxDialog
{
public:
    EditableTextureImageDialog(wxWindow* parent, TextureDisplacementLayer& layer, std::function<void()> refresh_model)
        : wxDialog(parent,wxID_ANY,_L("Adjust texture image"),wxDefaultPosition,wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_layer(layer),m_adjustment(layer),m_refresh_model(std::move(refresh_model)),m_timer(this)
    {
        auto* layout = new wxBoxSizer(wxVERTICAL);
        m_image = new wxStaticBitmap(this,wxID_ANY,wxNullBitmap);
        layout->Add(m_image,0,wxALL|wxALIGN_CENTER,10);
        auto* rows = new wxFlexGridSizer(3,8,8);
        rows->AddGrowableCol(1);
        const std::array<wxString,5> labels{_L("Brightness"),_L("Contrast"),_L("Sharpness"),_L("Blur"),_L("Hue")};
        for (size_t index=0;index<m_sliders.size();++index) {
            const int lo=index==0 || index==1 ? -1000 : index==4 ? -180 : 0;
            const int hi=index==4 ? 180 : 1000;
            rows->Add(new wxStaticText(this,wxID_ANY,labels[index]),0,wxALIGN_CENTER_VERTICAL);
            m_sliders[index] = new wxSlider(this,wxID_ANY,0,lo,hi,wxDefaultPosition,wxSize(300,-1));
            rows->Add(m_sliders[index],1,wxEXPAND);
            m_values[index] = new wxStaticText(this,wxID_ANY,"0.00",wxDefaultPosition,wxSize(55,-1));
            rows->Add(m_values[index],0,wxALIGN_CENTER_VERTICAL);
            m_sliders[index]->Bind(wxEVT_SLIDER,[this](wxCommandEvent&) { update_preview(); });
        }
        layout->Add(rows,0,wxALL|wxEXPAND,10);
        m_sliders[4]->Enable(m_adjustment.source().has_color());
        if (!m_adjustment.source().has_color())
            layout->Add(new wxStaticText(this,wxID_ANY,_L("This texture is grayscale, so it has no hue to shift.")),0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        auto* explanation = new wxStaticText(this,wxID_ANY,
            _L("Brightness, contrast and hue change the image first; blur and sharpness follow. The painted model updates after you stop moving a slider. Save as copy writes a new image to your texture library and uses it for this layer. Cancel restores the original image."));
        explanation->Wrap(450); layout->Add(explanation,0,wxALL,10);
        auto* commands = new wxBoxSizer(wxHORIZONTAL);
        auto* reset = new wxButton(this,wxID_RESET,_L("Reset"));
        auto* cancel = new wxButton(this,wxID_CANCEL,_L("Cancel"));
        m_save = new wxButton(this,wxID_OK,_L("Save as copy"));
        commands->Add(reset,0,wxRIGHT,6); commands->AddStretchSpacer();
        commands->Add(cancel,0,wxRIGHT,6); commands->Add(m_save);
        layout->Add(commands,0,wxALL|wxEXPAND,10);
        reset->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
            for (auto* slider:m_sliders) slider->SetValue(0);
            update_preview();
        });
        cancel->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) { cancel_edit(); });
        Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent&) { cancel_edit(); });
        m_save->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
            try {
                if (!m_adjustment.save_copy(m_layer)) return;
                m_finished = true; m_timer.Stop();
                if (m_refresh_model) m_refresh_model();
                EndModal(wxID_OK);
            } catch (const std::exception& error) {
                wxMessageBox(from_u8(error.what()),_L("Save texture image"),wxOK|wxICON_ERROR,this);
            }
        });
        Bind(wxEVT_TIMER,[this](wxTimerEvent&) { if (m_refresh_model) m_refresh_model(); },m_timer.GetId());
        update_preview(false);
        SetSizerAndFit(layout); CentreOnParent();
    }
    ~EditableTextureImageDialog() override
    {
        m_timer.Stop();
        if (!m_finished) {
            try { m_adjustment.cancel(m_layer); if (m_preview_touched && m_refresh_model) m_refresh_model(); }
            catch (...) {} // Parent handles the original exception; never throw during teardown.
        }
    }
private:
    void cancel_edit()
    {
        if (m_finished) return;
        m_timer.Stop();
        m_adjustment.cancel(m_layer);
        m_finished = true;
        if (m_preview_touched && m_refresh_model) m_refresh_model();
        EndModal(wxID_CANCEL);
    }
    void update_preview(bool notify = true)
    {
        auto& settings = m_adjustment.settings;
        settings.brightness = float(m_sliders[0]->GetValue())/1000.f;
        settings.contrast = float(m_sliders[1]->GetValue())/1000.f;
        settings.sharpness = float(m_sliders[2]->GetValue())/1000.f;
        settings.blur = float(m_sliders[3]->GetValue())/1000.f;
        settings.hue_deg = float(m_sliders[4]->GetValue());
        for (size_t i=0;i<m_values.size();++i)
            m_values[i]->SetLabel(i==4 ? wxString::Format("%d°",m_sliders[i]->GetValue()) :
                                      wxString::Format("%.2f",double(m_sliders[i]->GetValue())/1000.));
        if (notify) { m_preview_touched = true; m_adjustment.preview(m_layer); }
        const auto preview = m_adjustment.thumbnail();
        wxImage image(preview.width,preview.height);
        auto* pixels = image.GetData();
        if (preview.has_color()) std::copy(preview.rgb.begin(),preview.rgb.end(),pixels);
        else for (size_t pixel=0;pixel<preview.pixels.size();++pixel)
            pixels[pixel*3]=pixels[pixel*3+1]=pixels[pixel*3+2]=preview.pixels[pixel];
        // Small procedural textures can be only a few pixels wide. Enlarge the
        // display, not the sampled image, so their changes remain visible.
        const double display_scale = std::min(384. / preview.width, 192. / preview.height);
        image.Rescale(std::max(1, int(std::lround(preview.width * display_scale))),
                      std::max(1, int(std::lround(preview.height * display_scale))),
                      display_scale > 1. ? wxIMAGE_QUALITY_NORMAL : wxIMAGE_QUALITY_HIGH);
        m_image->SetBitmap(wxBitmap(image));
        m_save->Enable(!settings.identity());
        if (notify) m_timer.StartOnce(250); // Coalesce a drag before the actual displacement preview.
    }
    TextureDisplacementLayer& m_layer;
    TextureImageAdjustment m_adjustment;
    std::function<void()> m_refresh_model;
    wxTimer m_timer;
    wxStaticBitmap* m_image = nullptr;
    std::array<wxSlider*,5> m_sliders{};
    std::array<wxStaticText*,5> m_values{};
    wxButton* m_save = nullptr;
    bool m_finished = false, m_preview_touched = false;
};
