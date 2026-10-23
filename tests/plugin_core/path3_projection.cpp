#include "libslic3r/GCode/Path3Projection.hpp"
#include <cmath>
#include <stdexcept>

using namespace Slic3r;
void test_path3_projection()
{
    auto require = [](bool condition) { if (!condition) throw std::runtime_error("ordered contour projection failed"); };
    Polyline3 path(Points3{Vec3crd(0, 0, 0), Vec3crd(1000000, 1000000, 200000),
                   Vec3crd(0, 1000000, 400000), Vec3crd(1000000, 0, 600000)});
    Path3Projection projection(path);
    require(projection.z(Point(0, 0)) == 0.);
    require(std::abs(projection.z(Point(500000, 500000)) - .1) < 1e-8);
    require(std::abs(projection.z(Point(1000000, 1000000)) - .2) < 1e-8);
    require(std::abs(projection.z(Point(0, 1000000)) - .4) < 1e-8);
    require(std::abs(projection.z(Point(500000, 500000)) - .5) < 1e-8);
    require(std::abs(projection.z(Point(1000000, 0)) - .6) < 1e-8);
    bool rejected = false;
    try { projection.z(Point(0, 0)); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected);

    Polyline3 vertical(Points3{Vec3crd(0, 0, 0), Vec3crd(1000000, 0, 100000),
        Vec3crd(1000000, 0, 200000), Vec3crd(2000000, 0, 300000)});
    Path3Projection vertical_projection(vertical);
    require(vertical_projection.z(Point(0, 0)) == 0.);
    require(std::abs(vertical_projection.z(Point(1000000, 0)) - .1) < 1e-8);
    require(std::abs(vertical_projection.z(Point(1000000, 0)) - .2) < 1e-8);
    require(std::abs(vertical_projection.z(Point(1500000, 0)) - .25) < 1e-8);
    require(std::abs(vertical_projection.z(Point(2000000, 0)) - .3) < 1e-8);
    Path3Projection missing_vertical(vertical);
    missing_vertical.z(Point(0, 0));
    missing_vertical.z(Point(1000000, 0));
    rejected = false;
    try { missing_vertical.z(Point(2000000, 0)); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected);

    Polyline3 rounded(Points3{Vec3crd(123, 0, 100000), Vec3crd(123, 1000123, 200000),
        Vec3crd(123, 1000123, 300000), Vec3crd(123, 2000123, 400000)});
    Path3Projection rounded_projection(rounded);
    for (const auto& vertex : rounded.points) {
        const Point source(vertex.x(), vertex.y());
        require(std::abs(rounded_projection.z(scaled(unscaled(source))) - unscale<double>(vertex.z())) < 1e-8);
    }

    Polyline3 start_vertical(Points3{Vec3crd(0, 0, 100000), Vec3crd(0, 0, 200000), Vec3crd(1000000, 0, 300000)});
    Path3Projection start_projection(start_vertical);
    require(std::abs(start_projection.z(Point(0, 0)) - .1) < 1e-8);
    require(std::abs(start_projection.z(Point(0, 0)) - .2) < 1e-8);
    require(std::abs(start_projection.z(Point(1000000, 0)) - .3) < 1e-8);
}
