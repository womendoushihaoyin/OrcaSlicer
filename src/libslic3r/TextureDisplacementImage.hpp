#pragma once

// Image value/sampling subset of Orca TextureDisplacement at
// 14a1aa333b6da3ecadf853a4a6285b0f193b7605. It is independent of Model/painting/UV.
#include "Point.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
enum class TextureTileMethod : int { Repeat = 0, MirroredRepeat = 1 };

struct DecodedHeightTexture
{
    std::vector<uint8_t> pixels; // height: row-major, top-to-bottom, one byte per pixel
    // Colour: the same grid, three bytes per pixel, or empty when the source image was grayscale.
    // A grayscale height map has no colour to give - `pixels` is not a colour, it is a height - so
    // has_color() is what the whole colour feature keys off: a layer set to colour a model with a
    // grayscale texture on it simply colours nothing.
    std::vector<uint8_t> rgb;
    int width  = 0;
    int height = 0;

    bool empty() const { return width <= 0 || height <= 0 || pixels.size() != size_t(width) * size_t(height); }
    bool has_color() const { return !empty() && rgb.size() == size_t(width) * size_t(height) * 3; }
    // Bilinearly sampled height in [0, 1] at a normalized uv coordinate. When tile_enabled is false,
    // a uv outside [0, 1) samples as 0 - the texture simply is not there, rather than its border
    // row/column being smeared outward forever (which is what clamping the coordinate would do, and
    // was a real reported bug). Callers rely on this to get a hard edge: it is how the projection
    // frame's border becomes the edge of the displacement.
    float sample(const Vec2f &uv, bool tile_enabled = true, TextureTileMethod tile_method = TextureTileMethod::Repeat) const;
    // The same sample, in colour: linear RGB components in [0, 1]. Outside a non-tiled placement, and
    // for a grayscale source, this is (0, 0, 0) - callers pair it with has_color() and with the
    // height's own coverage rather than trying to read "no texture here" out of the colour itself.
    Vec3f sample_color(const Vec2f &uv, bool tile_enabled = true,
                       TextureTileMethod tile_method = TextureTileMethod::Repeat) const;

    // Where a uv lands on the texel grid: the four texels of the bilinear tap and their weights.
    // Shared by sample() and sample_color(), so a layer's height and its colour can never end up
    // read from different places in the image. False means the uv is outside a non-tiled placement -
    // no texture there at all (see sample()).
    struct TexelTap
    {
        int   x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        float tx = 0.f, ty = 0.f;
    };
    bool texel_tap(const Vec2f &uv, bool tile_enabled, TextureTileMethod tile_method, TexelTap &out) const;
};


// Fully validated 8-bit PNG snapshot. Invalid/corrupt/oversized data returns empty with an error.
DecodedHeightTexture decode_texture_image(const std::vector<unsigned char>& bytes, std::string& error);
} // namespace Slic3r
