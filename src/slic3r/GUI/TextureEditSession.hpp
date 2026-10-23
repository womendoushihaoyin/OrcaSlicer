#pragma once

#include "libslic3r/Model.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include <optional>

namespace Slic3r::GUI {

// Owned native data only. Preview workers never access the live model or plugins.
// The original mesh remains pinned until the session and its workers are gone.
struct TextureEditSession
{
    ObjectID volume_id;
    int instance_index = 0;
    std::shared_ptr<const TriangleMesh> source;
    Transform3d volume_to_world = Transform3d::Identity();
    std::vector<TextureDisplacementLayer> layers;
    TextureDisplacementOptions options;
    TextureDisplacementFacetsData masks;
    std::vector<Vec2f> model_uv;

    explicit TextureEditSession(const ModelVolume& volume, int selected_instance = 0);
    TextureDisplacementLayer* layer(int slot);
    const TextureDisplacementLayer* layer(int slot) const;
    int add_layer(const std::string& name, const std::string& path,
                  std::shared_ptr<std::vector<unsigned char>> image);
    bool remove_layer(int slot);
    bool move_layer(int slot, int direction);
    bool source_is_current(const ModelVolume& volume) const;
    void validate() const;

    // Persistent editing does not change topology or consume masks. Bake is an
    // explicit additional action: just as upstream, it retains layer parameters
    // and consumes the masks whose relief is now part of the mesh.
    void apply(ModelVolume& volume, std::optional<indexed_triangle_set> baked = std::nullopt,
               const std::vector<uint8_t>& texture_filaments = {}) const;
};

} // namespace Slic3r::GUI
