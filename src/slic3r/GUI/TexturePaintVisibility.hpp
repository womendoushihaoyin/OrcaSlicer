#pragma once
#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "TextureProjectionFrame.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Slic3r::GUI {
// Click-driven face selection for the editor's orthographic camera, matching the fixed upstream
// centroid-facing + real occlusion test. Partial visibility is sampled at each original face's
// centroid; this is not a per-pixel silhouette mask. The raycaster covers the source volume shown
// in this editor. Brush Circle/Sphere semantics are deliberately independent of this operation.
inline std::vector<int> texture_visible_faces(const TriangleMesh& source, const AABBMesh& raycaster,
                                              const Transform3d& to_display, const Vec3d& view_forward,
                                              const std::array<float,12>* frame_clip = nullptr)
{
    if (!to_display.matrix().allFinite() || !view_forward.allFinite() ||
        view_forward.squaredNorm() < 1e-20 || std::abs(to_display.linear().determinant()) < 1e-20)
        throw std::invalid_argument("Texture visibility needs an invertible display frame and camera direction");
    const auto& mesh = source.its;
    if (mesh.indices.empty()) return {};
    const Matrix3d inverse = to_display.linear().inverse();
    const Matrix3d normal_matrix = inverse.transpose();
    const Vec3d direction = (inverse * view_forward.normalized()).normalized();
    std::vector<int> result;
    for (size_t index = 0; index < mesh.indices.size(); ++index) {
        const auto& face = mesh.indices[index];
        const Vec3d a = mesh.vertices[size_t(face[0])].cast<double>();
        const Vec3d b = mesh.vertices[size_t(face[1])].cast<double>();
        const Vec3d c = mesh.vertices[size_t(face[2])].cast<double>();
        const Vec3d normal = normal_matrix * (b - a).cross(c - a);
        if (normal.squaredNorm() < 1e-20 || normal.dot(view_forward) >= 0.) continue;
        const Vec3d center = (a + b + c) / 3.;
        if (frame_clip && !texture_frame_contains(*frame_clip,(to_display * center).cast<float>())) continue;
        // Trace from the centroid toward the orthographic eye. AABBMesh stores hit.t as float;
        // recompute a candidate's plane intersection in double so coplanar ties stay visible and
        // a tiny but real front wall is not swallowed by a geometric epsilon or float distance.
        bool obscured = false;
        const Vec3d toward_camera = -direction;
        for (const auto& hit : raycaster.query_ray_hits(center, toward_camera)) {
            if (!hit.is_hit() || hit.face() == int(index)) continue;
            const auto& other = mesh.indices[size_t(hit.face())];
            const Vec3d oa = mesh.vertices[size_t(other[0])].cast<double>();
            const Vec3d ob = mesh.vertices[size_t(other[1])].cast<double>();
            const Vec3d oc = mesh.vertices[size_t(other[2])].cast<double>();
            const Vec3d plane_normal = (ob - oa).cross(oc - oa);
            const double denominator = plane_normal.dot(toward_camera);
            if (denominator == 0.) continue;
            const double distance = plane_normal.dot(oa - center) / denominator;
            const double roundoff = 64. * std::numeric_limits<double>::epsilon() *
                                    std::max({1., center.norm(), oa.norm()});
            if (distance > roundoff) { obscured = true; break; }
        }
        if (!obscured) result.push_back(int(index));
    }
    return result;
}

// Construct a replacement separately, so an empty view cannot erase existing editing data.
inline TriangleSelector::TriangleSplittingData texture_visible_mask(const TriangleMesh& source,
                                                                    const std::vector<int>& faces)
{
    if (faces.empty()) throw std::runtime_error("Nothing is visible from this angle; turn the model toward the surface to paint");
    TriangleSelector selector(source);
    for (int face : faces) {
        if (face < 0 || size_t(face) >= source.its.indices.size())
            throw std::invalid_argument("Texture visibility returned an invalid source face");
        selector.set_facet(face, EnforcerBlockerType::ENFORCER);
    }
    return selector.serialize();
}
} // namespace Slic3r::GUI
