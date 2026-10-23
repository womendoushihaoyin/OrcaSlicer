#pragma once

#include "libslic3r/Polyline.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Slic3r {

// The release quality estimator inserts XY samples without removing source
// vertices. Follow their source order, including self-intersections, to retain
// the contour Z offsets while preserving its speed and fan calculations.
class Path3Projection
{
public:
    explicit Path3Projection(const Polyline3& source) : m_source(source) {}

    double z(const Point& point)
    {
        const auto& points = m_source.points;
        if (!m_started) {
            if (points.empty() || !is_source_vertex(point, points.front()))
                throw std::runtime_error("Contoured path sampling must start at its first vertex");
            m_started = true;
            return unscale<double>(points.front().z());
        }
        for (; m_segment + 1 < points.size(); ++m_segment, m_fraction = 0.) {
            const Vec2d a = points[m_segment].head<2>().cast<double>();
            const Vec2d b = points[m_segment + 1].head<2>().cast<double>();
            const Vec2d delta = b - a;
            const double length2 = delta.squaredNorm();
            if (length2 == 0.) {
                if (!is_source_vertex(point, points[m_segment]))
                    throw std::runtime_error("Contoured path sampling omitted a vertical segment");
                // Its first vertex was consumed by the preceding segment (or
                // the initial sample). The repeated XY now denotes its end.
                return unscale<double>(points[++m_segment].z());
            }
            const double t = std::clamp((point.cast<double>() - a).dot(delta) / length2, 0., 1.);
            // Rounding an inserted XY sample to integer coordinates can move it
            // by one scaled unit. Do not silently project an unrelated point.
            if (t + 2. / std::sqrt(length2) >= m_fraction &&
                (a + t * delta - point.cast<double>()).squaredNorm() <= 4.) {
                m_fraction = std::max(m_fraction, t);
                const double result = unscale<double>(points[m_segment].z()) +
                    t * unscale<double>(points[m_segment + 1].z() - points[m_segment].z());
                if (is_source_vertex(point, points[m_segment + 1])) {
                    ++m_segment;
                    m_fraction = 0.;
                    return unscale<double>(points[m_segment].z());
                }
                return result;
            }
        }
        throw std::runtime_error("Contoured path sample is outside its ordered source geometry");
    }

private:
    static bool is_source_vertex(const Point& sample, const Vec3crd& vertex)
    {
        const Point source(vertex.x(), vertex.y());
        // The quality estimator stores millimeters, then truncates back to
        // scaled integers. Accept that exact representation as well as the
        // original, without mistaking nearby inserted samples for vertices.
        return sample == source || sample == scaled(unscaled(source));
    }
    const Polyline3& m_source;
    bool m_started = false;
    size_t m_segment = 0;
    double m_fraction = 0.;
};

} // namespace Slic3r
