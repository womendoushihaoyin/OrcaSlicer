#pragma once
#include "libslic3r/TextureDisplacement.hpp"
#include <stdexcept>

namespace Slic3r::GUI {
// Match the renderer's full clip transform, including the homogeneous divide.
// Viewport and gate use the same pixel units; gate origin is canvas top-left.
// The kernel samples in texture_displacement_bake_frame, so clip_from_bake must
// map that frame, not apply the volume transform a second time.
inline std::array<float,12> texture_frame_projection(const Eigen::Matrix4d& clip_from_bake,
                                                    const Vec2d& viewport, const Eigen::Vector4d& gate)
{
    if (!clip_from_bake.allFinite() || !viewport.allFinite() || !gate.allFinite() ||
        viewport.minCoeff() <= 0. || gate.z() < 1. || gate.w() < 1.)
        throw std::invalid_argument("The projection frame needs a finite, nonempty viewport and rectangle");
    Eigen::Matrix<double,3,4> result;
    result.row(0) = viewport.x()/(2.*gate.z())*clip_from_bake.row(0) +
                    (viewport.x()*.5-gate.x())/gate.z()*clip_from_bake.row(3);
    result.row(1) = -viewport.y()/(2.*gate.w())*clip_from_bake.row(1) +
                    (viewport.y()*.5-gate.y())/gate.w()*clip_from_bake.row(3);
    result.row(2) = clip_from_bake.row(3);
    std::array<float,12> matrix{};
    for (int row=0;row<3;++row)
        for (int column=0;column<4;++column) {
            const float value=float(result(row,column));
            if (!std::isfinite(value)) throw std::invalid_argument("The projection frame is outside the supported numeric range");
            matrix[size_t(row*4+column)]=value;
        }
    return matrix;
}

inline bool texture_frame_contains(const std::array<float,12>& matrix, const Vec3f& point)
{
    Vec2f uv;
    return project_uv_projective(matrix,point,uv) && uv.allFinite() &&
           uv.minCoeff() >= 0.f && uv.maxCoeff() <= 1.f;
}
} // namespace Slic3r::GUI
