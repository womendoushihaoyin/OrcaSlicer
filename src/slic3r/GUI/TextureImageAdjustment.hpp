#pragma once
#include "TextureEditSession.hpp"
#include "TextureLibrary.hpp"
#include <stdexcept>

namespace Slic3r::GUI {

// The source always excludes both the panel's temporary adjustment and the
// layer's live smoothing. Save as copy resolves the full-size result exactly once.
class TextureImageAdjustment
{
public:
    explicit TextureImageAdjustment(const TextureDisplacementLayer& layer)
        : m_source_image(layer.image_data), m_original_preview(layer.preview_adjust)
    {
        auto raw = layer;
        raw.smoothing = 0.f;
        raw.preview_adjust = {};
        m_source = decode_height_texture(raw);
        if (m_source.empty()) throw std::runtime_error("This layer has no usable image. Replace its image before adjusting it.");
        m_thumbnail = downscale_height_texture(m_source, 384);
    }
    TextureAdjustments settings;
    const DecodedHeightTexture& source() const { return m_source; }
    DecodedHeightTexture thumbnail() const { return apply_texture_adjustments(m_thumbnail, settings); }
    void preview(TextureDisplacementLayer& layer) const
    {
        check_source(layer);
        layer.preview_adjust = settings;
    }
    void cancel(TextureDisplacementLayer& layer) const
    {
        check_source(layer);
        layer.preview_adjust = m_original_preview;
    }
    bool save_copy(TextureDisplacementLayer& layer) const
    {
        check_source(layer);
        if (settings.identity()) { cancel(layer); return false; }
        const auto adjusted = apply_texture_adjustments(m_source, settings);
        std::string error;
        const auto entry = save_texture_to_library(adjusted, layer.name.empty() ? "texture" : layer.name, error);
        if (!entry) throw std::runtime_error(error);
        auto image = load_texture_image_data(*entry, error);
        if (!image) throw std::runtime_error(error);
        // Read and validate first: a failed save leaves the current layer usable.
        layer.preview_adjust = {};
        layer.name = entry->name;
        layer.path = entry->path;
        layer.path_in_3mf.clear();
        layer.image_data = std::move(image);
        touch_user_texture(entry->path);
        return true;
    }
private:
    void check_source(const TextureDisplacementLayer& layer) const
    {
        if (layer.image_data != m_source_image)
            throw std::runtime_error("The texture image changed. Close and reopen image adjustments.");
    }
    std::shared_ptr<std::vector<unsigned char>> m_source_image;
    TextureAdjustments m_original_preview;
    DecodedHeightTexture m_source, m_thumbnail;
};
} // namespace Slic3r::GUI
