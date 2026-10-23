#include "TextureDisplacementConsumer.hpp"
#include "TextureBake/TextureBakeMesh.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace Slic3r {

TextureDisplacementResult build_texture_displacement_mesh(const indexed_triangle_set& input,
                                                         const TextureDisplacementRequest& request,
                                                         const TextureBake::PipelineProgressFn& progress)
{
    TextureDisplacementResult result;
    if (progress && !progress("Decode", 0.0)) {
        result.canceled = true;
        return result;
    }
    if (!request.image_data || request.image_data->empty()) {
        result.error = "No texture image was selected.";
        return result;
    }
    if (!std::isfinite(request.depth_mm) || !std::isfinite(request.tiling_scale) || request.tiling_scale <= 0.f ||
        !std::isfinite(request.rotation_deg) || !request.offset.allFinite() || !std::isfinite(request.zero_level) ||
        !std::isfinite(request.edge_length_mm) || request.edge_length_mm < 0.01 || request.max_triangles > 5000000 ||
        request.zero_level < 0.f || request.zero_level > 1.f) {
        result.error = "Invalid texture displacement settings.";
        return result;
    }
    if (input.indices.empty() || input.vertices.empty() || input.indices.size() > 5000000 ||
        (!request.face_excluded.empty() && request.face_excluded.size() != input.indices.size())) {
        result.error = "The selected mesh or texture face mask is invalid or too large.";
        return result;
    }
    TextureBake::DisplaceBounds bounds;
    bounds.min = bounds.max = input.vertices.front();
    for (const auto& vertex : input.vertices) {
        if (!vertex.allFinite()) {
            result.error = "The selected mesh contains non-finite coordinates.";
            return result;
        }
        bounds.min = bounds.min.cwiseMin(vertex);
        bounds.max = bounds.max.cwiseMax(vertex);
    }
    for (const auto& face : input.indices)
        for (int i = 0; i < 3; ++i)
            if (face[i] < 0 || size_t(face[i]) >= input.vertices.size()) {
                result.error = "The selected mesh contains an invalid triangle index.";
                return result;
            }

    const DecodedHeightTexture image = decode_texture_image(*request.image_data, result.error);
    if (image.empty())
        return result;

    // Orca 14a1aa3: UVTransform, box_plane_{yz,xz,xy}, |normal|^4 blend and Rec.601
    // height pixels. Coordinates and depth are in the caller's mesh units (normally world mm).
    const float scale = 1.f / request.tiling_scale;
    const float radians = request.rotation_deg * float(M_PI) / 180.f;
    const float cs = std::cos(radians), sn = std::sin(radians);
    const float aspect = float(image.width) / float(image.height);
    const auto sample_at = [&](Vec2f p) {
        p *= scale;
        const Vec2f uv = Vec2f(p.x() * cs - p.y() * sn, (p.x() * sn + p.y() * cs) * aspect) + request.offset;
        // Outside an untiled placement there is no relief, including when inverted or centered.
        if (!request.tile_enabled && (uv.x() < 0.f || uv.x() >= 1.f || uv.y() < 0.f || uv.y() >= 1.f))
            return 0.f;
        float value = image.sample(uv, request.tile_enabled, request.tile_method);
        if (request.invert)
            value = 1.f - value;
        return (value - request.zero_level) * request.depth_mm;
    };
    const auto sample = [&](const Vec3f& p, const Vec3f&, const Vec3f& n) {
        const auto sign = [](float v) { return v >= 0.f ? 1.f : -1.f; };
        Vec3f weights = n.cwiseAbs();
        weights = weights.cwiseProduct(weights);
        weights = weights.cwiseProduct(weights);
        const float sum = weights.sum();
        if (sum < 1e-8f)
            return sample_at(Vec2f(sign(n.z()) * p.x(), -p.y()));
        weights /= sum;
        return weights.x() * sample_at(Vec2f(sign(n.x()) * p.y(), -p.z())) +
               weights.y() * sample_at(Vec2f(-sign(n.y()) * p.x(), -p.z())) +
               weights.z() * sample_at(Vec2f(sign(n.z()) * p.x(), -p.y()));
    };

    TextureBake::PipelineSettings settings;
    settings.refine_length = request.edge_length_mm;
    settings.max_triangles = request.max_triangles;
    settings.harvest_tol = std::clamp(settings.refine_length / 32.0, 0.005, 0.05);
    settings.preserve_untextured = true;
    settings.clamp_below_plate = true;
    settings.displace.amplitude = 1.f;
    settings.displace.symmetric = false;
    settings.displace.bottom_angle_limit = 0.f;
    settings.displace.top_angle_limit = 0.f;

    indexed_triangle_set oriented = input;
    if (request.flip_normals)
        for (auto& triangle : oriented.indices)
            std::swap(triangle[1], triangle[2]);
    try {
        const auto mode = request.max_triangles > 0 ? TextureBake::PipelineMode::Export : TextureBake::PipelineMode::Bake;
        auto baked = TextureBake::run_pipeline(TextureBake::to_soup(oriented, request.face_excluded), sample, settings, bounds,
                                              mode, request.face_excluded, progress);
        result.canceled = baked.canceled;
        result.safety_cap_hit = baked.safety_cap_hit;
        result.budget_limited = baked.budget_limited;
        if (result.canceled)
            return result;
        result.mesh = TextureBake::to_indexed_triangle_set(baked.geometry);
        if (request.flip_normals)
            for (auto& triangle : result.mesh.indices)
                std::swap(triangle[1], triangle[2]);
        if (result.mesh.indices.empty())
            result.error = "Texture baking produced an empty mesh.";
    } catch (const std::exception& ex) {
        result.error = std::string("Texture baking failed: ") + ex.what();
    }
    return result;
}

} // namespace Slic3r
