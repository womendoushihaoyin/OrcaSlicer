#pragma once

#include "TextureDisplacementImage.hpp"
#include "TriangleMesh.hpp"
#include "TextureBake/TextureBakePipeline.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

// Minimal host consumer for the fixed Orca texture resource contract. The image is a detached
// snapshot; a GUI job owns this request and its input mesh, never a Python object or a ModelVolume*.
// Uses upstream triplanar sampling and the complete upstream TextureBake pipeline. Layer painting,
// UV editing and editable layer serialization are not represented by this bounded consumer.
struct TextureDisplacementRequest
{
    std::shared_ptr<const std::vector<unsigned char>> image_data;
    float depth_mm       = 0.4f;
    float tiling_scale   = 12.5f;
    float rotation_deg   = 0.f;
    Vec2f offset         = Vec2f::Zero();
    float zero_level     = 0.f;
    bool  invert         = false;
    bool  tile_enabled   = true;
    TextureTileMethod tile_method = TextureTileMethod::Repeat;

    double edge_length_mm = 0.5;
    size_t max_triangles = 750000;
    bool flip_normals = false;
    // One byte per triangle, or empty to texture the whole mesh. Nonzero means unchanged.
    std::vector<uint8_t> face_excluded;
};

struct TextureDisplacementResult
{
    indexed_triangle_set mesh;
    bool canceled = false;
    bool safety_cap_hit = false;
    bool budget_limited = false;
    std::string error;
    bool success() const { return !canceled && error.empty() && !mesh.indices.empty(); }
};

// Preview and commit use the same function/settings. The caller publishes the resulting mesh only
// if its model/selection generation still matches and the job has not been canceled.
TextureDisplacementResult build_texture_displacement_mesh(const indexed_triangle_set& input,
                                                         const TextureDisplacementRequest& request,
                                                         const TextureBake::PipelineProgressFn& progress = {});

} // namespace Slic3r
