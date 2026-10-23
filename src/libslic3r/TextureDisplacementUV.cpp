#include "TextureDisplacementUV.hpp"
#include "AABBTreeIndirect.hpp"
#include <limits>
#include <cmath>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
namespace Slic3r {
// Barycentric weights of `p` inside triangle (a, b, c), as the pair that belongs with the edges
// (b - a) and (c - a): p ~= (1 - w1 - w2) * a + w1 * b + w2 * c. `p` is projected onto the triangle's
// plane, so a point a hair off the surface still reports the weights of its foot point. A degenerate
// triangle reports its centroid, the only coordinate that means anything there.
static void barycentric_weights(const Vec3f &a, const Vec3f &b, const Vec3f &c, const Vec3f &p, float &w1, float &w2)
{
    const Vec3f e0 = b - a, e1 = c - a, ep = p - a;
    const float d00 = e0.dot(e0), d01 = e0.dot(e1), d11 = e1.dot(e1), dp0 = ep.dot(e0), dp1 = ep.dot(e1);
    const float den = d00 * d11 - d01 * d01;
    if (std::abs(den) <= 1e-20f) {
        w1 = w2 = 1.f / 3.f;
        return;
    }
    w1 = (d11 * dp0 - d01 * dp1) / den;
    w2 = (d00 * dp1 - d01 * dp0) / den;
}

std::vector<Vec2f> compute_model_corner_uvs(const indexed_triangle_set &base_mesh,
                                            const std::vector<Vec2f>   &model_uv,
                                            const indexed_triangle_set &patch,
                                            const std::vector<int>     &patch_source)
{
    if (patch.indices.empty() || model_uv.size() != base_mesh.indices.size() * 3 ||
        patch_source.size() != patch.indices.size())
        return {};

    std::vector<Vec2f> corner(patch.indices.size() * 3, Vec2f::Zero());
    for (size_t j = 0; j < patch.indices.size(); ++j) {
        const int S = patch_source[j];
        if (S < 0 || size_t(S) >= base_mesh.indices.size())
            continue; // no source triangle: leave (0, 0), which the callers read as "no placement"
        const stl_triangle_vertex_indices &src = base_mesh.indices[size_t(S)];
        const Vec3f &a = base_mesh.vertices[size_t(src[0])];
        const Vec3f &b = base_mesh.vertices[size_t(src[1])];
        const Vec3f &c = base_mesh.vertices[size_t(src[2])];
        const Vec2f *uv = &model_uv[size_t(S) * 3];
        const stl_triangle_vertex_indices &tri = patch.indices[j];
        for (int k = 0; k < 3; ++k) {
            // An unsplit patch triangle is its source triangle, so its corners land exactly on a, b
            // and c and this reproduces their uvs unchanged; a sub-triangle the brush cut gets the
            // interpolated coordinate, which is the whole point of going through the barycentric.
            float w1 = 0.f, w2 = 0.f;
            barycentric_weights(a, b, c, patch.vertices[size_t(tri[k])], w1, w2);
            corner[j * 3 + size_t(k)] = (1.f - w1 - w2) * uv[0] + w1 * uv[1] + w2 * uv[2];
        }
    }
    return corner;
}

std::vector<Vec2f> collapse_corner_uvs(const indexed_triangle_set &patch, const std::vector<Vec2f> &corner)
{
    if (patch.indices.empty() || corner.size() != patch.indices.size() * 3)
        return {};

    std::vector<Vec2f> per_vertex(patch.vertices.size(), Vec2f::Zero());
    std::vector<char>  seen(patch.vertices.size(), 0);
    for (size_t j = 0; j < patch.indices.size(); ++j)
        for (int k = 0; k < 3; ++k) {
            const int vi = patch.indices[j][k];
            if (vi < 0 || size_t(vi) >= per_vertex.size() || seen[size_t(vi)])
                continue;
            seen[size_t(vi)]       = 1;
            per_vertex[size_t(vi)] = corner[j * 3 + size_t(k)];
        }
    return per_vertex;
}

std::vector<Vec2f> transfer_uv_channel(const indexed_triangle_set &src_mesh,
                                       const std::vector<Vec2f>   &src_uv,
                                       const indexed_triangle_set &dst_mesh,
                                       float                       max_distance)
{
    if (src_mesh.indices.empty() || dst_mesh.indices.empty() || src_uv.size() != src_mesh.indices.size() * 3)
        return {};

    const AABBTreeIndirect::Tree3f tree =
        AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(src_mesh.vertices, src_mesh.indices);
    const float limit2 = max_distance >= 0.f ? max_distance * max_distance : std::numeric_limits<float>::max();

    std::vector<Vec2f> out(dst_mesh.indices.size() * 3, Vec2f::Zero());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, dst_mesh.indices.size()),
                      [&](const tbb::blocked_range<size_t> &range) {
        for (size_t j = range.begin(); j < range.end(); ++j) {
            const stl_triangle_vertex_indices &tri = dst_mesh.indices[j];
            for (int k = 0; k < 3; ++k) {
                const Vec3f &p = dst_mesh.vertices[size_t(tri[k])];
                size_t       hit = 0;
                Vec3f        foot;
                const float  d2 = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
                    src_mesh.vertices, src_mesh.indices, tree, p, hit, foot);
                if (d2 < 0.f || d2 > limit2 || hit >= src_mesh.indices.size())
                    continue; // nowhere near the source surface: (0, 0), as documented
                const stl_triangle_vertex_indices &st = src_mesh.indices[hit];
                float w1 = 0.f, w2 = 0.f;
                barycentric_weights(src_mesh.vertices[size_t(st[0])], src_mesh.vertices[size_t(st[1])],
                                    src_mesh.vertices[size_t(st[2])], foot, w1, w2);
                const Vec2f *uv        = &src_uv[hit * 3];
                out[j * 3 + size_t(k)] = (1.f - w1 - w2) * uv[0] + w1 * uv[1] + w2 * uv[2];
            }
        }
    });
    return out;
}

std::vector<uint8_t> transfer_facet_paint(const indexed_triangle_set                    &src_mesh,
                                         const TriangleSelector::TriangleSplittingData &src_paint,
                                         const indexed_triangle_set                    &dst_mesh)
{
    if (src_mesh.indices.empty() || dst_mesh.indices.empty() || src_paint.bitstream.empty())
        return {};

    const TriangleMesh src(src_mesh);
    TriangleSelector   selector(src);
    selector.deserialize(src_paint, false);

    // Every sub-triangle of the source, each carrying its own state - NONE included, so the soup
    // covers the whole surface and a foot point always lands on something. One soup rather than one
    // tree per state: a foot point has one nearest triangle, and that triangle's state is the answer.
    std::vector<indexed_triangle_set> per_state;
    selector.get_facets(per_state);
    indexed_triangle_set soup;
    std::vector<uint8_t> label;
    for (size_t st = 0; st < per_state.size(); ++st) {
        const indexed_triangle_set &part = per_state[st];
        if (part.indices.empty())
            continue;
        const int base = int(soup.vertices.size());
        soup.vertices.insert(soup.vertices.end(), part.vertices.begin(), part.vertices.end());
        for (const stl_triangle_vertex_indices &t : part.indices) {
            soup.indices.emplace_back(t[0] + base, t[1] + base, t[2] + base);
            label.push_back(uint8_t(st));
        }
    }
    if (soup.indices.empty())
        return {};

    const AABBTreeIndirect::Tree3f tree =
        AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(soup.vertices, soup.indices);

    std::vector<uint8_t> out(dst_mesh.indices.size(), 0);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, dst_mesh.indices.size()),
                      [&](const tbb::blocked_range<size_t> &range) {
        for (size_t j = range.begin(); j < range.end(); ++j) {
            const stl_triangle_vertex_indices &t = dst_mesh.indices[j];
            // The centroid, not a corner: a facet takes one state, and its middle is the point that
            // is inside it whatever the triangle looks like.
            const Vec3f centroid = (dst_mesh.vertices[size_t(t[0])] + dst_mesh.vertices[size_t(t[1])] +
                                    dst_mesh.vertices[size_t(t[2])]) / 3.f;
            size_t      hit = 0;
            Vec3f       foot;
            const float d2 = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(soup.vertices, soup.indices,
                                                                                        tree, centroid, hit, foot);
            if (d2 >= 0.f && hit < label.size())
                out[j] = label[hit];
        }
    });
    return out;
}


} // namespace Slic3r
