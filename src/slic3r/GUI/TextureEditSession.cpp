#include "TextureEditSession.hpp"
#include "libslic3r/TextureDisplacementUV.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {
Transform3d selected_volume_to_world(const ModelVolume& volume, int instance_index)
{
    const auto* object = volume.get_object();
    if (!object || object->instances.empty()) return volume.get_matrix();
    if (instance_index < 0 || size_t(instance_index) >= object->instances.size() || !object->instances[size_t(instance_index)])
        throw std::invalid_argument("The selected model instance no longer exists");
    return object->instances[size_t(instance_index)]->get_matrix() * volume.get_matrix();
}
void validate_mask(const TriangleSelector::TriangleSplittingData& mask, size_t face_count)
{
    std::set<int> faces;
    for (const auto& triangle : mask.triangles_to_split) {
        if (triangle.triangle_idx < 0 || size_t(triangle.triangle_idx) >= face_count ||
            !faces.insert(triangle.triangle_idx).second || triangle.bitstream_start_idx < 0)
            throw std::invalid_argument("A texture mask does not match its mesh");
        size_t bit = size_t(triangle.bitstream_start_idx);
        auto nibble = [&]() {
            if (bit > mask.bitstream.size() || mask.bitstream.size() - bit < 4)
                throw std::invalid_argument("A texture mask has truncated triangle data");
            int value = 0;
            for (int i = 0; i < 4; ++i) value |= int(mask.bitstream[bit++]) << i;
            return value;
        };
        size_t pending = 1;
        while (pending != 0) {
            --pending;
            const int code = nibble(), sides = code & 3;
            if (sides != 0) {
                if ((code >> 2) > 2)
                    throw std::invalid_argument("A texture mask has an invalid split edge");
                pending += size_t(sides + 1);
            } else if ((code >> 2) > int(EnforcerBlockerType::BLOCKER)) {
                throw std::invalid_argument("A texture mask contains an unsupported paint state");
            }
        }
    }
}
void validate_mesh(const indexed_triangle_set& mesh)
{
    if (mesh.indices.empty() || mesh.vertices.empty() ||
        std::any_of(mesh.vertices.begin(), mesh.vertices.end(), [](const Vec3f& v) { return !v.allFinite(); }))
        throw std::invalid_argument("The texture surface is empty or has invalid vertices");
    for (const auto& face : mesh.indices)
        for (int corner = 0; corner < 3; ++corner)
            if (face[corner] < 0 || size_t(face[corner]) >= mesh.vertices.size())
                throw std::invalid_argument("The texture surface has an invalid triangle index");
}
} // namespace


TextureEditSession::TextureEditSession(const ModelVolume& volume, int selected_instance)
    : volume_id(volume.id()), instance_index(selected_instance), source(volume.mesh_ptr()),
      volume_to_world(selected_volume_to_world(volume, selected_instance)),
      layers(volume.texture_displacement_layers), options(volume.texture_displacement_options), model_uv(volume.texture_uv)
{
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
        masks[size_t(slot)] = volume.texture_displacement_facet(slot).get_data();
    std::sort(layers.begin(), layers.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
    validate();
}

TextureDisplacementLayer* TextureEditSession::layer(int slot)
{
    auto found = std::find_if(layers.begin(), layers.end(), [slot](const auto& layer) { return layer.slot == slot; });
    return found == layers.end() ? nullptr : &*found;
}

const TextureDisplacementLayer* TextureEditSession::layer(int slot) const
{
    return const_cast<TextureEditSession*>(this)->layer(slot);
}

int TextureEditSession::add_layer(const std::string& name, const std::string& path,
                                  std::shared_ptr<std::vector<unsigned char>> image)
{
    if (!image || image->empty())
        throw std::invalid_argument("A texture layer needs image data");
    int slot = 0;
    while (slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS) && layer(slot))
        ++slot;
    if (slot == int(TEXTURE_DISPLACEMENT_MAX_LAYERS))
        throw std::runtime_error("A part can contain at most eight texture layers");
    TextureDisplacementLayer added;
    added.slot = slot;
    added.name = name;
    added.path = path;
    added.image_data = std::move(image);
    if (model_uv.size() == source->its.indices.size() * 3)
        added.projection_method = TextureProjectionMethod::ModelUV;
    layers.push_back(std::move(added));
    std::sort(layers.begin(), layers.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
    masks[size_t(slot)] = {};
    options.active_slot = slot;
    return slot;
}

bool TextureEditSession::remove_layer(int slot)
{
    auto found = std::find_if(layers.begin(), layers.end(), [slot](const auto& layer) { return layer.slot == slot; });
    if (found == layers.end())
        return false;
    layers.erase(found);
    masks[size_t(slot)] = {};
    options.active_slot = layers.empty() ? 0 : layers.front().slot;
    return true;
}

bool TextureEditSession::move_layer(int slot, int direction)
{
    if (direction != -1 && direction != 1)
        return false;
    auto found = std::find_if(layers.begin(), layers.end(), [slot](const auto& layer) { return layer.slot == slot; });
    if (found == layers.end() || (direction < 0 && found == layers.begin()) ||
        (direction > 0 && found + 1 == layers.end()))
        return false;
    auto adjacent = found + direction;
    const int new_slot = adjacent->slot;
    std::swap(found->slot, adjacent->slot);
    std::swap(masks[size_t(slot)], masks[size_t(new_slot)]);
    std::iter_swap(found, adjacent);
    options.active_slot = new_slot;
    return true;
}

bool TextureEditSession::source_is_current(const ModelVolume& volume) const
{
    if (const auto* object = volume.get_object(); object && !object->instances.empty() &&
        (instance_index < 0 || size_t(instance_index) >= object->instances.size() || !object->instances[size_t(instance_index)]))
        return false;
    return volume.id() == volume_id && volume.mesh_ptr() == source &&
           selected_volume_to_world(volume, instance_index).matrix().isApprox(volume_to_world.matrix(), 1e-12);
}

void TextureEditSession::validate() const
{
    if (!source) throw std::invalid_argument("The selected part has no surface");
    validate_mesh(source->its);
    if (!volume_to_world.matrix().allFinite() || std::abs(volume_to_world.linear().determinant()) < 1e-12)
        throw std::invalid_argument("The selected part has an invalid transform");
    std::array<bool, TEXTURE_DISPLACEMENT_MAX_LAYERS> seen{};
    for (const auto& entry : layers) {
        if (entry.slot < 0 || size_t(entry.slot) >= seen.size() || seen[size_t(entry.slot)])
            throw std::invalid_argument("Invalid or duplicate texture layer slot");
        seen[size_t(entry.slot)] = true;
        if (!std::isfinite(entry.depth_mm) || !std::isfinite(entry.tiling_scale) || entry.tiling_scale <= 0.f ||
            !std::isfinite(entry.rotation_deg) || !entry.offset.allFinite() ||
            !std::isfinite(entry.midlevel) || !std::isfinite(entry.smoothing) ||
            !std::isfinite(entry.edge_smoothing_amount) || !std::isfinite(entry.lscm_seam_angle_deg) ||
            !std::isfinite(entry.island_padding_mm) || !entry.view_project_right.allFinite() || !entry.view_project_up.allFinite() ||
            std::any_of(entry.view_project_matrix.begin(), entry.view_project_matrix.end(), [](float v) { return !std::isfinite(v); }))
            throw std::invalid_argument("Texture parameters must be finite");
        if (int(entry.projection_method) < 0 || int(entry.projection_method) > int(TextureProjectionMethod::ModelUV) ||
            int(entry.blend_mode) < 0 || int(entry.blend_mode) > int(TextureBlendMode::Override) ||
            int(entry.tile_method) < 0 || int(entry.tile_method) > int(TextureTileMethod::MirroredRepeat))
            throw std::invalid_argument("The texture uses an unknown projection, blend or repeat mode");
        for (const auto& island : entry.islands)
            if (!island.offset.allFinite() || !std::isfinite(island.rotation_deg) || !std::isfinite(island.scale) || island.scale <= 0.f)
                throw std::invalid_argument("A UV island has an invalid transform");
        for (const auto& override : entry.lscm_uv_overrides)
            if (!override.second.allFinite()) throw std::invalid_argument("A UV override is not finite");
        for (const auto& seam : entry.lscm_seam_edges)
            if (seam.first < 0 || seam.second < 0 || size_t(seam.first) >= source->its.vertices.size() ||
                size_t(seam.second) >= source->its.vertices.size())
                throw std::invalid_argument("A UV seam belongs to a different mesh");
    }
    for (const auto& mask : masks) validate_mask(mask, source->its.indices.size());
    if (!std::isfinite(options.v2_refine_mm) || options.v2_refine_mm < 0.f || options.v2_max_triangles_k < -1 ||
        !std::isfinite(options.smooth_strength) || options.smooth_iterations < 0)
        throw std::invalid_argument("The texture bake options are invalid");
    if (!model_uv.empty() && model_uv.size() != source->its.indices.size() * 3)
        throw std::invalid_argument("The model UV channel does not match its triangles");
    if (std::any_of(model_uv.begin(), model_uv.end(), [](const Vec2f& uv) { return !uv.allFinite(); }))
        throw std::invalid_argument("The model UV channel is not finite");
}

void TextureEditSession::apply(ModelVolume& volume, std::optional<indexed_triangle_set> baked,
                               const std::vector<uint8_t>& texture_filaments) const
{
    validate();
    if (!baked && !texture_filaments.empty())
        throw std::invalid_argument("Texture colors require a completed bake");
    if (!source_is_current(volume))
        throw std::runtime_error("The part changed while the texture editor was open");
    std::array<std::unique_ptr<TriangleSelector>, TEXTURE_DISPLACEMENT_MAX_LAYERS> selectors;
    for (size_t slot = 0; slot < masks.size(); ++slot) {
        selectors[slot] = std::make_unique<TriangleSelector>(*source);
        selectors[slot]->deserialize(masks[slot]);
    }
    // Compute all transfer data before changing the live mesh.
    std::array<std::vector<uint8_t>, 4> transferred;
    std::array<std::vector<uint8_t>, TEXTURE_DISPLACEMENT_MAX_LAYERS> surviving_masks;
    std::array<bool, TEXTURE_DISPLACEMENT_MAX_LAYERS> consumed{};
    std::vector<Vec2f> uv = model_uv;
    bool topology_changed = false, geometry_changed = false;
    if (baked) {
        validate_mesh(*baked);
        geometry_changed = baked->vertices != source->its.vertices || baked->indices != source->its.indices;
        if (!texture_filaments.empty() && texture_filaments.size() != baked->indices.size())
            throw std::invalid_argument("Texture colors do not match the baked triangles");
        if (!geometry_changed) {
            bool color_changed = false;
            if (!texture_filaments.empty()) {
                TriangleSelector selector(*source);
                selector.deserialize(volume.mmu_segmentation_facets.get_data());
                const auto before = selector.serialize();
                for (size_t face = 0; face < texture_filaments.size(); ++face)
                    if (texture_filaments[face]) selector.set_facet(int(face), EnforcerBlockerType(texture_filaments[face]));
                color_changed = selector.serialize() != before;
            }
            if (!color_changed)
                throw std::runtime_error("The texture bake changed neither geometry nor color; editable masks were preserved");
        }
        for (size_t slot = 0; slot < masks.size(); ++slot) {
            const auto* entry = layer(int(slot));
            consumed[slot] = entry && TriangleSelector::has_facets(masks[slot], EnforcerBlockerType::ENFORCER) &&
                             !decode_height_texture(*entry).empty();
        }
        if (texture_filaments.empty() && std::any_of(layers.begin(), layers.end(), [](const auto& layer) { return layer.color_enabled; }))
            throw std::runtime_error("The bake has no color result; editable color settings were preserved");
        topology_changed = geometry_changed && (options.pipeline_v2 || baked->indices != source->its.indices);
        if (topology_changed) {
            const std::array<const FacetsAnnotation*, 4> original{
                &volume.supported_facets, &volume.seam_facets, &volume.mmu_segmentation_facets, &volume.fuzzy_skin_facets};
            for (size_t channel = 0; channel < original.size(); ++channel)
                transferred[channel] = transfer_facet_paint(source->its, original[channel]->get_data(), *baked);
            if (!uv.empty())
                uv = transfer_uv_channel(source->its, uv, *baked);
            for (size_t slot = 0; slot < masks.size(); ++slot)
                if (!consumed[slot] && !masks[slot].bitstream.empty())
                    surviving_masks[slot] = transfer_facet_paint(source->its, masks[slot], *baked);
        }
    }
    volume.texture_displacement_layers = layers;
    volume.texture_displacement_options = options;
    for (size_t slot = 0; slot < masks.size(); ++slot)
        volume.texture_displacement_facet(int(slot)).set(*selectors[slot]);
    if (baked) {
        if (geometry_changed) volume.set_mesh(TriangleMesh(std::move(*baked)));
        if (topology_changed) {
            const std::array<FacetsAnnotation*, 4> target{
                &volume.supported_facets, &volume.seam_facets, &volume.mmu_segmentation_facets, &volume.fuzzy_skin_facets};
            for (size_t channel = 0; channel < target.size(); ++channel) {
                TriangleSelector selector(volume.mesh());
                for (size_t triangle = 0; triangle < transferred[channel].size(); ++triangle)
                    if (transferred[channel][triangle] != 0)
                        selector.set_facet(int(triangle), EnforcerBlockerType(transferred[channel][triangle]));
                target[channel]->set(selector);
            }
        }
        if (!texture_filaments.empty()) {
            TriangleSelector selector(volume.mesh());
            selector.deserialize(volume.mmu_segmentation_facets.get_data());
            for (size_t face = 0; face < texture_filaments.size(); ++face)
                if (texture_filaments[face])
                    selector.set_facet(int(face), EnforcerBlockerType(texture_filaments[face]));
            volume.mmu_segmentation_facets.set(selector);
        }
        for (size_t slot = 0; slot < masks.size(); ++slot) {
            auto& annotation = volume.texture_displacement_facet(int(slot));
            if (consumed[slot]) {
                annotation.reset();
            } else if (topology_changed) {
                TriangleSelector selector(volume.mesh());
                for (size_t triangle = 0; triangle < surviving_masks[slot].size(); ++triangle)
                    if (surviving_masks[slot][triangle] != 0)
                        selector.set_facet(int(triangle), EnforcerBlockerType(surviving_masks[slot][triangle]));
                annotation.set(selector);
            }
        }
        if (geometry_changed) {
            volume.calculate_convex_hull();
            volume.invalidate_convex_hull_2d();
            volume.set_new_unique_id();
            if (auto* object = volume.get_object()) object->invalidate_bounding_box();
        }
        if (topology_changed) {
            // Mesh-index seam keys and chart-index edits cannot describe the new topology.
            // Keep texture parameters/images, just as with the consumed painted masks.
            for (auto& entry : volume.texture_displacement_layers) {
                entry.lscm_seam_edges.clear(); entry.lscm_uv_overrides.clear();
                entry.islands.clear(); entry.island_groups.clear();
            }
        }
    }
    volume.texture_uv = std::move(uv);
}

} // namespace Slic3r::GUI
