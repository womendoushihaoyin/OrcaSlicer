#ifndef SLIC3R_TEXTURE_DISPLACEMENT_PAINT_HPP
#define SLIC3R_TEXTURE_DISPLACEMENT_PAINT_HPP

#include "TriangleSelector.hpp"
#include <utility>
#include <vector>

namespace Slic3r {
// Extract the painted patch and resolve original mesh seam edges to its actual brush-generated
// sub-edges. `seams` is a working copy: replaced with patch-index edges. Keep the persistent layer's
// original endpoint pairs unchanged. Resolution follows the split tree, including T-joints, never
// geometric proximity. The two-argument extraction remains declared in TextureDisplacement.hpp.
indexed_triangle_set extract_painted_patch(const indexed_triangle_set                    &base_mesh,
                                            const TriangleSelector::TriangleSplittingData &facet_data,
                                            std::vector<std::pair<int, int>>               &seams);
// Canonical endpoint pairs, sorted by patch_edge. Each actual painted patch edge is returned at
// most once, only when its split-tree ancestry lies on an original mesh edge. Patch indices match
// extract_painted_patch(), including preserved unused original vertex slots. Internal subdivision
// edges have no original-edge ancestor and are intentionally absent. Coincident positions do not
// imply shared topology. Persist original_edge only; patch_edge may contain temporary brush IDs.
struct TexturePatchEdgeOrigin {
    std::pair<int, int> patch_edge;
    std::pair<int, int> original_edge;
};
std::vector<TexturePatchEdgeOrigin> texture_patch_edge_origins(
    const indexed_triangle_set &base_mesh, const TriangleSelector::TriangleSplittingData &facet_data);
} // namespace Slic3r

#endif
