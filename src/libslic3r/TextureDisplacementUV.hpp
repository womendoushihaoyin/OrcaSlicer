#pragma once
// Independent UV/paint transfer from Orca 14a1aa333b6da3ecadf853a4a6285b0f193b7605.
#include "TriangleMesh.hpp"
#include "TriangleSelector.hpp"
#include <vector>
namespace Slic3r {
// `model_uv` is the volume's channel: three uvs per triangle of `base_mesh`, in its triangle order
// (see ModelVolume::texture_uv). `patch_source` maps each patch triangle back to the base-mesh
// triangle it was cut from - TriangleSelector::get_facets_strict()'s out_source. A patch triangle is
// always a piece of exactly one base triangle, so each of its corners is interpolated barycentrically
// inside that triangle's three uvs. That is what keeps a sub-triangle the brush split (whose corners
// are not base-mesh vertices at all) on the texture, and what keeps the two sides of a UV seam
// reading from their own island instead of from one averaged coordinate.
//
// Returns an empty vector when the channel does not match `base_mesh`, when `patch_source` does not
// match `patch`, or when either is empty - the callers treat that as "no placement" and fall back,
// exactly as they do for an unwrap that failed.
std::vector<Vec2f> compute_model_corner_uvs(const indexed_triangle_set &base_mesh,
                                            const std::vector<Vec2f>   &model_uv,
                                            const indexed_triangle_set &patch,
                                            const std::vector<int>     &patch_source);

// One UV per patch vertex, collapsed from a per-corner array: the first corner to reach a vertex wins.
// This is the per-corner/per-vertex split compute_lscm_uvs() documents, applied to any corner array -
// displacement is per-vertex, so a vertex on a seam has to settle on one of its copies.
// Returns an empty vector if `corner` is not three per patch triangle.
std::vector<Vec2f> collapse_corner_uvs(const indexed_triangle_set &patch, const std::vector<Vec2f> &corner);

// Resamples a per-corner UV channel from `src_mesh` onto `dst_mesh`, for when something replaces a
// volume's mesh and the model's own UV channel has to come along: the colour import's painted mesh,
// the gizmo's remesh/subdivide, the default bake pipeline's rebuild. Each destination corner takes the
// uv of its closest point on the source surface, interpolated barycentrically inside the source
// triangle it lands on - the same transfer LscmLookup does per point, run over every corner.
//
// Exact wherever the two meshes describe the same surface, which is the case for every caller here
// (they subdivide, repair or displace it, they do not reshape it). A corner that lands nowhere near
// the source - farther than `max_distance` (negative: no limit) - takes (0, 0).
//
// One thing a positional transfer cannot reproduce: a *seam* vertex, where one position carries a
// different uv in each island that meets there. Position alone does not say which island a corner
// belongs to, so corners on a seam all take whichever island the nearest-triangle lookup lands on,
// and the texture can be off by up to one triangle in a band along the island borders. Doing better
// needs a triangle-to-triangle correspondence, which only some of the stages that replace a mesh
// have (a remesh has none at all).
//
// Returns an empty vector when `src_uv` is not three per source triangle, or when either mesh is empty.
std::vector<Vec2f> transfer_uv_channel(const indexed_triangle_set &src_mesh,
                                       const std::vector<Vec2f>   &src_uv,
                                       const indexed_triangle_set &dst_mesh,
                                       float                       max_distance = -1.f);

// Resample all serialized facet states onto a replacement topology using nearest-face centroids.
std::vector<uint8_t> transfer_facet_paint(const indexed_triangle_set& src_mesh,
                                         const TriangleSelector::TriangleSplittingData& src_paint,
                                         const indexed_triangle_set& dst_mesh);

} // namespace Slic3r
