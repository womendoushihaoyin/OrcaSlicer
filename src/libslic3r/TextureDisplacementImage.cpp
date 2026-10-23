#include "TextureDisplacementImage.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <png.h>

namespace Slic3r {
bool DecodedHeightTexture::texel_tap(const Vec2f &uv, bool tile_enabled, TextureTileMethod tile_method,
                                     TexelTap &tap) const
{
    if (empty() || !std::isfinite(uv.x()) || !std::isfinite(uv.y()))
        return false;

    auto repeat01 = [](float x) {
        x = std::fmod(x, 1.f);
        return x < 0.f ? x + 1.f : x;
    };
    // Standard mirrored-repeat: reflect back and forth every other unit, so tile edges always
    // line up with themselves instead of jumping from one edge of the image to the other.
    auto mirrored_repeat01 = [](float x) {
        x = std::fmod(std::abs(x), 2.f);
        return x > 1.f ? 2.f - x : x;
    };

    if (!tile_enabled && (uv.x() < 0.f || uv.x() >= 1.f || uv.y() < 0.f || uv.y() >= 1.f))
        // Outside the single, non-repeating placement entirely: no texture there, not "smeared
        // edge pixel" - clamping the *coordinate* to [0, 1] would otherwise keep returning the
        // border row/column's height forever in every direction, stretching it out to infinity.
        return false;

    float u, v;
    if (!tile_enabled) {
        u = uv.x();
        v = uv.y();
    } else if (tile_method == TextureTileMethod::MirroredRepeat) {
        u = mirrored_repeat01(uv.x());
        v = mirrored_repeat01(uv.y());
    } else {
        u = repeat01(uv.x());
        v = repeat01(uv.y());
    }

    const float fx = u * float(width);
    const float fy = v * float(height);
    const int   x0 = std::clamp(int(std::floor(fx)), 0, width - 1);
    const int   y0 = std::clamp(int(std::floor(fy)), 0, height - 1);
    // Neighbour for bilinear filtering: wrap for tiling methods, clamp at the edge otherwise (a
    // repeating neighbour would incorrectly blend against the opposite edge of the image).
    tap.x0 = x0;
    tap.y0 = y0;
    tap.x1 = tile_enabled ? (x0 + 1) % width  : std::min(x0 + 1, width - 1);
    tap.y1 = tile_enabled ? (y0 + 1) % height : std::min(y0 + 1, height - 1);
    tap.tx = fx - std::floor(fx);
    tap.ty = fy - std::floor(fy);
    return true;
}

float DecodedHeightTexture::sample(const Vec2f &uv, bool tile_enabled, TextureTileMethod tile_method) const
{
    TexelTap tap;
    if (!texel_tap(uv, tile_enabled, tile_method, tap))
        return 0.f;
    const int   x0 = tap.x0, y0 = tap.y0, x1 = tap.x1, y1 = tap.y1;
    const float tx = tap.tx, ty = tap.ty;

    auto at = [this](int x, int y) { return float(pixels[size_t(y) * size_t(width) + size_t(x)]) / 255.f; };
    const float top    = at(x0, y0) * (1.f - tx) + at(x1, y0) * tx;
    const float bottom = at(x0, y1) * (1.f - tx) + at(x1, y1) * tx;
    return top * (1.f - ty) + bottom * ty;
}

Vec3f DecodedHeightTexture::sample_color(const Vec2f &uv, bool tile_enabled, TextureTileMethod tile_method) const
{
    TexelTap tap;
    if (!has_color() || !texel_tap(uv, tile_enabled, tile_method, tap))
        return Vec3f::Zero();

    auto at = [this](int x, int y) {
        const size_t i = (size_t(y) * size_t(width) + size_t(x)) * 3;
        return Vec3f(float(rgb[i]) / 255.f, float(rgb[i + 1]) / 255.f, float(rgb[i + 2]) / 255.f);
    };
    const Vec3f top    = at(tap.x0, tap.y0) * (1.f - tap.tx) + at(tap.x1, tap.y0) * tap.tx;
    const Vec3f bottom = at(tap.x0, tap.y1) * (1.f - tap.tx) + at(tap.x1, tap.y1) * tap.tx;
    return top * (1.f - tap.ty) + bottom * tap.ty;
}


DecodedHeightTexture decode_texture_image(const std::vector<unsigned char>& bytes, std::string& error)
{
    error.clear();
    DecodedHeightTexture result;
    // Validate signature/IHDR before allocations. 8-bit matches the fixed upstream sampler contract.
    if (bytes.size() < 33 || png_sig_cmp(bytes.data(), 0, 8) || bytes[24] != 8) {
        error = "The texture is not an 8-bit PNG image.";
        return result;
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) {
        error = "Could not decode the texture image.";
        png_image_free(&image);
        return result;
    }
    struct ImageGuard {
        png_image& image;
        ~ImageGuard() { png_image_free(&image); }
    } guard{image};
    constexpr uint64_t max_pixels = 64ULL * 1024 * 1024;
    if (image.width == 0 || image.height == 0 || image.width > 32768 || image.height > 32768 ||
        uint64_t(image.width) * image.height > max_pixels) {
        error = "The texture image dimensions exceed the supported limit.";
        return result;
    }
    const bool has_color = (image.format & PNG_FORMAT_FLAG_COLOR) != 0;
    const bool has_alpha = (image.format & PNG_FORMAT_FLAG_ALPHA) != 0;
    // Keep the source RGB/gray values when stripping alpha, matching the upstream importer. Asking
    // libpng to remove alpha would composite against a background and alter height at transparent pixels.
    image.format = has_color ? (has_alpha ? PNG_FORMAT_RGBA : PNG_FORMAT_RGB)
                             : (has_alpha ? PNG_FORMAT_GA : PNG_FORMAT_GRAY);
    std::vector<unsigned char> decoded(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, decoded.data(), 0, nullptr)) {
        error = "The texture image is damaged or incomplete.";
        return result;
    }
    result.width = int(image.width);
    result.height = int(image.height);
    if (!has_color) {
        if (!has_alpha)
            result.pixels = std::move(decoded);
        else {
            result.pixels.resize(size_t(result.width) * size_t(result.height));
            for (size_t i = 0; i < result.pixels.size(); ++i)
                result.pixels[i] = decoded[i * 2];
        }
    } else {
        if (!has_alpha)
            result.rgb = std::move(decoded);
        else {
            result.rgb.resize(size_t(result.width) * size_t(result.height) * 3);
            for (size_t i = 0; i < size_t(result.width) * size_t(result.height); ++i)
                std::copy_n(decoded.data() + i * 4, 3, result.rgb.data() + i * 3);
        }
        result.pixels.resize(size_t(result.width) * size_t(result.height));
        for (size_t i = 0; i < result.pixels.size(); ++i)
            result.pixels[i] = uint8_t(std::lround(0.299 * result.rgb[i * 3] +
                                                 0.587 * result.rgb[i * 3 + 1] + 0.114 * result.rgb[i * 3 + 2]));
    }
    return result;
}
} // namespace Slic3r
