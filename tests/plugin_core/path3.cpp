#include "libslic3r/ExtrusionEntity.hpp"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

using namespace Slic3r;

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<class Function> void require_stale_geometry_failure(Function function)
{
    bool failed = false;
    try {
        function();
    } catch (const std::logic_error& error) {
        failed = std::string(error.what()).find("XY geometry changed") != std::string::npos;
    }
    require(failed, "stale XY geometry was silently accepted as a 3D path");
}

void geometry()
{
    Polyline3 empty, first, second;
    empty.clip_end(1000);
    empty.simplify(10);
    require(empty.empty() && !empty.remove_duplicate_points(), "empty 3D path changed");
    require(!empty.split_at_index(0, &first, &second), "empty path split succeeded");

    Polyline3 line(Points3{Vec3crd(0, 0, 0), Vec3crd(30000, 40000, 120000)});
    require(std::abs(line.length() - 130000.) < 1.e-6, "3D length ignored Z");
    require(line.split_at_length(65000., &first, &second), "3D split failed");
    require(first.last_point() == Vec3crd(15000, 20000, 60000), "length split did not interpolate Z");
    require(first.last_point() == second.first_point(), "split endpoints disagree");
    require(std::abs(first.length() + second.length() - line.length()) < 1.e-6, "split changed length");
    Point query(15000, 20000);
    line.split_at(query, &first, &second);
    require(first.last_point().z() == 60000 && second.first_point().z() == 60000, "XY split lost Z");
    Polyline3 clipped = line;
    clipped.clip_end(65000.);
    require(clipped.last_point() == Vec3crd(15000, 20000, 60000), "3D clip ignored Z distance");
    clipped.clip_start(32500.);
    require(clipped.first_point() == Vec3crd(7500, 10000, 30000), "3D start clip failed");
    clipped.clip_end(1.e9);
    require(clipped.empty(), "over-clipping must empty the path");

    Polyline3 vertical(Points3{Vec3crd(10, 20, 0), Vec3crd(10, 20, 10000)});
    Vec3crd spatial_query(10, 20, 2500);
    vertical.split_at(spatial_query, &first, &second);
    require(first.last_point().z() == 2500 && second.first_point().z() == 2500, "vertical segment split failed");

    Polyline3 hump(Points3{Vec3crd(0, 0, 0), Vec3crd(10000, 0, 10000), Vec3crd(20000, 0, 0)});
    hump.simplify_by_fitting_arc(100.);
    require(hump.size() == 3 && hump.points[1].z() == 10000, "arc fitting flattened a Z feature");
    require(hump.fitting_result.empty(), "nonplanar path received XY arc metadata");
    hump.reverse();
    require(hump.first_point() == Vec3crd(20000, 0, 0), "3D reverse failed");
    hump.translate(Vec3crd(10, 20, 30));
    hump.rotate(0., 1.);
    require(hump.points[1] == Vec3crd(-20, 10010, 10030), "XY rotation changed Z");

    Polyline circle;
    for (size_t i = 0; i <= 32; ++i) {
        const double angle = double(i) * M_PI / 64.;
        circle.points.emplace_back(coord_t(std::round(100000. * std::cos(angle))), coord_t(std::round(100000. * std::sin(angle))));
    }
    Polyline3 elevated(circle, 7000);
    elevated.simplify_by_fitting_arc(50.);
    require(elevated.is_valid(), "planar arc simplification removed the path");
    for (const Vec3crd& point : elevated.points)
        require(point.z() == 7000, "planar arc fitting reset Z to zero");
    elevated.split_at_index(elevated.size() / 2, &first, &second);
    require(first.last_point().z() == 7000 && second.first_point().z() == 7000, "arc split reset Z");
    first.append(second);
    require(first.first_point().z() == 7000 && first.last_point().z() == 7000, "append lost elevation");
}

void extrusion()
{
    ExtrusionPath planar(erExternalPerimeter, 0.08, 0.4f, 0.2f, true);
    planar.inset_idx = 3;
    planar.set_reverse();
    planar.polyline = Polyline{Point(0, 0), Point(30000, 40000)};
    require(!planar.has_polyline3() && planar.points3().back().z() == 0, "planar snapshot must have zero Z offsets");
    ExtrusionPath path = planar;
    path.set_polyline3(Polyline3(Points3{Vec3crd(0, 0, 0), Vec3crd(30000, 40000, 120000)}));
    require(path.polyline.points.back() == Point(30000, 40000), "3D setter did not synchronize XY");
    require(std::abs(path.length() - 130000.) < 1.e-6, "extrusion length ignored Z");
    require(std::abs(path.total_volume() - 0.08 * unscale<double>(130000.)) < 1.e-10, "extrusion volume ignored Z");
    std::unique_ptr<ExtrusionEntity> clone(path.clone());
    const auto* cloned = dynamic_cast<const ExtrusionPath*>(clone.get());
    require(cloned && cloned->polyline3()->last_point().z() == 120000, "clone lost 3D geometry");
    require(cloned->inset_idx == 3 && cloned->is_force_no_extrusion() && !cloned->can_reverse(), "clone lost Snapmaker process fields");
    ExtrusionPath moved(std::move(path));
    moved.reverse();
    require(moved.polyline3()->first_point().z() == 120000 && moved.first_point() == Point(30000, 40000), "extrusion reverse desynchronized XY/Z");
    moved.clip_end(65000.);
    require(moved.polyline3()->last_point().z() == 60000 && moved.last_point() == Point(15000, 20000), "extrusion clipping lost Z");
    ExtrusionPath first, second;
    require(moved.split_at_length(32500., &first, &second), "extrusion split failed");
    require(first.polyline3()->last_point().z() == 90000 && second.polyline3()->first_point().z() == 90000, "extrusion split lost Z");
    require(first.inset_idx == 3 && first.is_force_no_extrusion() && !first.can_reverse(), "split lost Snapmaker process fields");
    first.append(second);
    require(std::abs(first.length() - moved.length()) < 1.e-6, "extrusion append changed 3D length");
    first.simplify_by_fitting_arc(1.);
    require(first.polyline3()->first_point().z() == 120000 && first.polyline3()->last_point().z() == 60000, "extrusion simplification flattened Z");

    ExtrusionPath stale = moved;
    stale.polyline.points.back().x() += 1;
    require_stale_geometry_failure([&] { stale.points3(); });
    require_stale_geometry_failure([&] { stale.length(); });
    require_stale_geometry_failure([&] { ExtrusionPath copy(stale); });
    stale.set_polyline(Polyline{Point(1, 2), Point(3, 4)});
    require(!stale.has_polyline3() && stale.points3().front().z() == 0, "explicit planar replacement did not reset Z");

    ExtrusionPath ring = planar;
    ring.set_polyline3(Polyline3(Points3{
        Vec3crd(0, 0, 0), Vec3crd(scale_(10.), 0, scale_(0.2)),
        Vec3crd(scale_(10.), scale_(10.), scale_(0.4)), Vec3crd(0, 0, 0)}));
    ExtrusionLoop loop(ring);
    loop.split_at(Point::new_scale(5., 0.), false, 0.);
    require(loop.paths.front().polyline3()->first_point().z() == coord_t(scale_(0.1)), "seam split did not interpolate contour Z");
    require(loop.paths.back().polyline3()->last_point().z() == coord_t(scale_(0.1)), "seam split broke contour closure");
    require(loop.paths.front().inset_idx == 3 && loop.paths.front().is_force_no_extrusion(), "seam split lost process fields");
    loop.reverse();
    ExtrusionPaths clipped;
    loop.clip_end(scale_(0.1), &clipped);
    require(!clipped.empty() && clipped.back().has_polyline3(), "loop clip dropped 3D geometry");
    ExtrusionLoopSloped scarf(clipped, scale_(0.05), 1., scale_(0.2), 0.2, elrDefault);
    require(!scarf.starts.empty() && !scarf.ends.empty(), "3D scarf paths were not built");
    for (const ExtrusionPath* segment : scarf.get_all_paths()) {
        require(segment->has_polyline3(), "scarf generation lost contour geometry");
        require(segment->inset_idx == 3, "scarf generation lost inset_idx");
    }
    scarf.clip_slope(scale_(0.01));
    for (const ExtrusionPath* segment : scarf.get_all_paths())
        segment->points3();

    ExtrusionPath odd_planar = planar;
    odd_planar.polyline = Polyline{Point(0, 0), Point(5, 3)};
    ExtrusionPaths odd_paths{odd_planar};
    ExtrusionLoopSloped planar_scarf(odd_paths, 0., unscale<double>(odd_planar.length()), 4., 0.2, elrDefault);
    require(planar_scarf.starts.size() == 1 && !planar_scarf.starts.front().has_polyline3(), "ordinary scarf gained 3D storage");
    require(planar_scarf.starts.front().polyline.points.at(1) == Point(2, 1), "ordinary scarf changed release midpoint rounding");
}
}

void test_path3(const std::string& scenario)
{
    if (scenario == "path3_geometry")
        geometry();
    else if (scenario == "path3_extrusion")
        extrusion();
    else
        throw std::runtime_error("Unknown path3 test scenario: " + scenario);
}
