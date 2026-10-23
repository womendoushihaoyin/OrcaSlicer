#pragma once
#include "TextureEditSession.hpp"

namespace Slic3r::GUI {
struct TexturePaletteEntry
{
    int filament_id = 0; // Actual one-based model/MMU id, never a sidebar display index.
    Vec3f rgb = Vec3f::Zero();
    bool mixed = false;
};
using TexturePalette = std::vector<TexturePaletteEntry>;

// Fixed-upstream 24^3 CIEDE2000 lookup, with preference for a pure filament
// unless an existing mixed filament improves the match by at least Delta E 2.
ColorQuantizeFn make_texture_palette_quantizer(const TexturePalette& palette, bool pure_only = false,
                                               const DisplacementProgressFn& progress = {});

struct TextureColorBakeResult
{
    indexed_triangle_set mesh;
    // 0 preserves the previous paint; >0 is an actual existing filament id.
    std::vector<uint8_t> texture_filaments;
    // Actual merged MMU colors shown by the preview, including prior paint.
    std::vector<uint8_t> preview_filaments;
};
TextureColorBakeResult bake_texture_session(const TextureEditSession& session, const TexturePalette& palette,
    const TriangleSelector::TriangleSplittingData& existing_mmu, const DisplacementProgressFn& progress = {});
} // namespace Slic3r::GUI
