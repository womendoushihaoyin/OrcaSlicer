#pragma once
#include "TextureEditSession.hpp"
#include <functional>
class wxWindow;
namespace Slic3r::GUI {
struct TextureMixCommit;
struct TextureMixedPaletteSnapshot;
struct EditableTextureResult
{
    TextureEditSession session;
    std::optional<indexed_triangle_set> baked;
    std::vector<uint8_t> texture_filaments;
    std::shared_ptr<TextureMixCommit> mixed_commit;
    std::shared_ptr<const TextureMixedPaletteSnapshot> palette_before;
};
// Cancel never touches the live model. The caller validates identity, snapshots,
// applies the result, then notifies the normal model-change/reslice path.
std::optional<EditableTextureResult> show_editable_texture_dialog(
    wxWindow* parent, const ModelVolume& volume, const std::function<bool()>& source_is_current, int instance_index = 0);
}
