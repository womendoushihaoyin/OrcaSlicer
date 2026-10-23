// Z contouring algorithm adapted from OrcaSlicer
// 14a1aa333b6da3ecadf853a4a6285b0f193b7605 for the release_2.4.1 path representation.
#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "SLA/IndexedMesh.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace {

void contour_path(LayerRegion& region, const sla::IndexedMesh& mesh, ExtrusionPath& path)
{
    if (path.role() != erTopSolidInfill && path.role() != erIroning && !is_perimeter(path.role()))
        return;
    if (path.role() == erOverhangPerimeter || path.size() < 2)
        return;
    const Layer& layer = *region.layer();
    const auto& config = region.region().config();
    const double minimum_height = config.zaa_min_z.value;
    if (!std::isfinite(minimum_height) || minimum_height < 0 || minimum_height > layer.height + EPSILON)
        throw SlicingError("Z contouring minimum Z height exceeds the layer height");

    // slice_z and the indexed mesh share model coordinates. print_z also has a
    // raft/object lift; subtract it so only the contour offset is written to Z.
    const double model_print_z = layer.print_z - layer.object()->slicing_parameters().object_print_z_min;
    const double ray_z = layer.slice_z;
    constexpr double resolution_mm = 0.1;
    const double slope_threshold = config.zaa_minimize_perimeter_height.value;
    const double upper = path.role() == erIroning ? layer.height : minimum_height;
    const double lower = path.role() == erIroning ? -(layer.height + 0.1) : -(layer.height - minimum_height);
    const Points& source = path.polyline.points;
    Pointf3s sampled;
    std::vector<bool> original_vertices;
    bool changed = false;

    for (size_t segment = 1; segment < source.size(); ++segment) {
        if (layer.object()->print()->canceled())
            throw CanceledException();
        const Vec2d start = source[segment - 1].cast<double>() * SCALING_FACTOR;
        const Vec2d delta = (source[segment].cast<double>() - source[segment - 1].cast<double>()) * SCALING_FACTOR;
        const size_t count = size_t(std::ceil(delta.norm() / resolution_mm));
        if (count == 0)
            continue;
        for (size_t i = 0; i <= count; ++i) {
            const Vec2d point = start + delta * (double(i) / double(count));
            const auto hit = mesh.query_ray_hit(Vec3d(point.x(), point.y(), ray_z), Vec3d::UnitZ());
            double offset = hit.is_hit() ? hit.distance() - (model_print_z - ray_z) : 0.;
            if (hit.is_hit() && is_perimeter(path.role()) && offset > lower && slope_threshold > 0) {
                const double slope = std::acos(std::clamp(std::abs(hit.normal().normalized().z()), 0., 1.));
                if (slope * 180. / M_PI > slope_threshold)
                    offset = std::max(lower, offset - 0.5 * path.width * std::sin(slope));
            }
            if (offset < -layer.height || offset > upper + 0.03)
                offset = 0.;
            offset = std::clamp(offset, lower, upper);
            if (is_perimeter(path.role()) && offset > 0)
                offset = 0.;
            changed |= std::abs(offset) > EPSILON;
            const Vec3d candidate(point.x(), point.y(), offset);
            const bool original = i == 0 || i == count;
            if (!sampled.empty() && (sampled.back() - candidate).squaredNorm() < EPSILON * EPSILON) {
                original_vertices.back() = original_vertices.back() || original;
                continue;
            }
            // Keep source vertices even when collinear; closed paths must retain
            // their topology for seam placement and polygon consumers.
            if (sampled.size() >= 2 && !original_vertices.back()) {
                const Vec3d direction = sampled.back() - sampled[sampled.size() - 2];
                const Vec3d distance = candidate - sampled[sampled.size() - 2];
                const double squared = direction.squaredNorm();
                if (squared > 0 && distance.cross(direction).squaredNorm() / squared < EPSILON * EPSILON) {
                    sampled.back() = candidate;
                    original_vertices.back() = original;
                    continue;
                }
            }
            sampled.push_back(candidate);
            original_vertices.push_back(original);
        }
    }
    if (changed && sampled.size() >= 2) {
        Polyline3 result;
        result.points.reserve(sampled.size());
        for (const Vec3d& point : sampled)
            result.append((point / SCALING_FACTOR).array().round().cast<coord_t>().matrix());
        path.set_polyline3(std::move(result));
    }
}

void contour_entity(LayerRegion& region, const sla::IndexedMesh& mesh, ExtrusionEntity& entity)
{
    if (auto* collection = dynamic_cast<ExtrusionEntityCollection*>(&entity)) {
        for (ExtrusionEntity* child : collection->entities)
            contour_entity(region, mesh, *child);
    } else if (auto* loop = dynamic_cast<ExtrusionLoopSloped*>(&entity)) {
        for (ExtrusionPath& path : loop->starts) contour_path(region, mesh, path);
        for (ExtrusionPath& path : loop->paths) contour_path(region, mesh, path);
        for (ExtrusionPath& path : loop->ends) contour_path(region, mesh, path);
    } else if (auto* loop = dynamic_cast<ExtrusionLoop*>(&entity)) {
        for (ExtrusionPath& path : loop->paths) contour_path(region, mesh, path);
    } else if (auto* multipath = dynamic_cast<ExtrusionMultiPath*>(&entity)) {
        for (ExtrusionPath& path : multipath->paths) contour_path(region, mesh, path);
    } else if (auto* path = dynamic_cast<ExtrusionPath*>(&entity)) {
        contour_path(region, mesh, *path);
    } else {
        throw SlicingError("Z contouring encountered an unsupported extrusion entity");
    }
}
}

void Layer::make_contour_z(const sla::IndexedMesh& mesh)
{
    // Preserve the planar bed contact of the first object layer.
    if (lower_layer == nullptr)
        return;
    for (LayerRegion* region : regions()) {
        if (!region->region().config().zaa_enabled)
            continue;
        contour_entity(*region, mesh, region->fills);
        contour_entity(*region, mesh, region->perimeters);
    }
}
}
