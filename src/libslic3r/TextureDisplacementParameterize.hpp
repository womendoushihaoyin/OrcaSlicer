// Migrated from Orca 14a1aa333b6da3ecadf853a4a6285b0f193b7605.
#pragma once
#include "TriangleMesh.hpp"
#include <optional>
#include <vector>

namespace Slic3r {
// A genuine CGAL least-squares conformal map for one topological disk. Returns no map for
// a closed, disconnected, multiply bounded or invalid patch, preserving upstream fallback.
std::optional<std::vector<Vec2f>> parameterize_texture_lscm(const indexed_triangle_set &mesh);
}
