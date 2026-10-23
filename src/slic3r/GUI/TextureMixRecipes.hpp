#pragma once
#include "TextureColorBake.hpp"
#include "libslic3r/MixedFilament.hpp"

namespace Slic3r::GUI {

struct TextureMixPolicy
{
    int gradient_mode = 0;
    bool advanced_dithering = false;
};

struct TextureMixRecipe
{
    MixedFilament row;
    int preview_filament_id = 0;
    int existing_filament_id = 0;
};

// A preview owns a complete independent manager. Its candidate IDs are real
// slots in that manager, but must never be applied to the live Model directly.
// resolve_texture_mix_commit() allocates only the recipes the bake actually used
// and remaps every output face to the resulting committed manager's IDs.
struct TextureMixPlan
{
    TexturePalette palette;
    TexturePalette preview_palette;
    std::vector<TextureMixRecipe> recipes;
    MixedFilamentManager source_manager;
    MixedFilamentManager preview_manager;
    MixedFilamentDisplayContext context;
    TextureMixPolicy policy;
    bool canceled = false;
};

TextureMixPlan plan_texture_mix_palette(const std::vector<TextureDisplacementLayer>& layers,
    const TexturePalette& existing_palette, const MixedFilamentManager& manager,
    const MixedFilamentDisplayContext& context, TextureMixPolicy policy, int requested_count,
    const DisplacementProgressFn& progress = {});

struct TextureMixCommit
{
    MixedFilamentManager manager;
    TextureColorBakeResult bake;
    std::vector<uint64_t> created_stable_ids;
    std::string definitions;
};

// No live state is changed on success, failure, or cancellation. The GUI must
// validate its palette snapshot, then commit this result and the Model together.
TextureMixCommit resolve_texture_mix_commit(const TextureMixPlan& plan, TextureColorBakeResult bake);

} // namespace Slic3r::GUI
