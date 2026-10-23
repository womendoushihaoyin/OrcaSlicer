// Palette matching adapted from Orca 14a1aa333b6da3ecadf853a4a6285b0f193b7605.
#include "TextureColorBake.hpp"
#include "TextureMixRecipes.hpp"
#include "libslic3r/Color.hpp"
#include <map>
#include <numeric>
#include "libslic3r/TextureDisplacementUV.hpp"
#include "slic3r/Utils/ColorSpaceConvert.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {
Vec3f srgb_to_lab(const Vec3f& c)
{
    const auto lin = [](float v) { return v > .04045f ? std::pow((v + .055f) / 1.055f, 2.4f) : v / 12.92f; };
    const float r = lin(c.x()), g = lin(c.y()), b = lin(c.z());
    const float x = (.4124f * r + .3576f * g + .1805f * b) / .95047f;
    const float y = .2126f * r + .7152f * g + .0722f * b;
    const float z = (.0193f * r + .1192f * g + .9505f * b) / 1.08883f;
    const auto f = [](float t) { return t > .008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
    return Vec3f(116.f * f(y) - 16.f, 500.f * (f(x) - f(y)), 200.f * (f(y) - f(z)));
}
void validate_palette(const TexturePalette& palette)
{
    if (palette.empty() || palette.size() > 255)
        throw std::invalid_argument("Color baking needs 1 to 255 existing filament colors");
    std::set<int> ids;
    bool pure = false;
    for (const auto& entry : palette) {
        if (entry.filament_id < 1 || entry.filament_id > 255 || !ids.insert(entry.filament_id).second ||
            !entry.rgb.allFinite() || entry.rgb.minCoeff() < 0.f || entry.rgb.maxCoeff() > 1.f)
            throw std::invalid_argument("The color bake palette has an invalid or duplicate filament");
        pure |= !entry.mixed;
    }
    if (!pure) throw std::invalid_argument("Color baking needs at least one physical filament");
}
} // namespace

ColorQuantizeFn make_texture_palette_quantizer(const TexturePalette& palette, bool pure_only,
                                               const DisplacementProgressFn& progress)
{
    validate_palette(palette);
    std::vector<Vec3f> lab;
    lab.reserve(palette.size());
    for (const auto& entry : palette) lab.push_back(srgb_to_lab(entry.rgb));
    constexpr int edge = 24;
    auto lookup = std::make_shared<std::vector<uint8_t>>(edge * edge * edge, 0);
    for (int r = 0; r < edge; ++r) {
        if (progress && !progress(r * 100 / edge)) return {};
        for (int g = 0; g < edge; ++g)
            for (int b = 0; b < edge; ++b) {
                const Vec3f sample = srgb_to_lab(Vec3f((r + .5f) / edge, (g + .5f) / edge, (b + .5f) / edge));
                float best_distance = std::numeric_limits<float>::max(), pure_distance = best_distance;
                int best = -1, best_pure = -1;
                for (size_t i = 0; i < palette.size(); ++i) {
                    if (pure_only && palette[i].mixed) continue;
                    const float distance = DeltaE00(sample.x(), sample.y(), sample.z(), lab[i].x(), lab[i].y(), lab[i].z());
                    if (distance < best_distance) { best_distance = distance; best = int(i); }
                    if (!palette[i].mixed && distance < pure_distance) { pure_distance = distance; best_pure = int(i); }
                }
                if (best_pure >= 0 && palette[size_t(best)].mixed && pure_distance - best_distance < 2.f)
                    best = best_pure;
                (*lookup)[(size_t(r) * edge + size_t(g)) * edge + size_t(b)] = uint8_t(best);
            }
    }
    return [lookup](const Vec3f& rgb) {
        if (!rgb.allFinite()) return -1;
        const int r = std::clamp(int(std::clamp(rgb.x(), 0.f, 1.f) * edge), 0, edge - 1);
        const int g = std::clamp(int(std::clamp(rgb.y(), 0.f, 1.f) * edge), 0, edge - 1);
        const int b = std::clamp(int(std::clamp(rgb.z(), 0.f, 1.f) * edge), 0, edge - 1);
        return int((*lookup)[(size_t(r) * edge + size_t(g)) * edge + size_t(b)]);
    };
}

TextureColorBakeResult bake_texture_session(const TextureEditSession& session, const TexturePalette& palette,
    const TriangleSelector::TriangleSplittingData& existing_mmu, const DisplacementProgressFn& progress)
{
    session.validate();
    TextureColorBakeResult result;
    const bool wants_color = std::any_of(session.layers.begin(), session.layers.end(), [](const auto& layer) { return layer.color_enabled; });
    TextureColorRequest request;
    std::vector<uint8_t> palette_indices;
    if (wants_color) {
        const auto preparing = [&progress](int percent) { return !progress || progress(percent / 20); };
        request.quantize = make_texture_palette_quantizer(palette, !session.options.color_mix_enabled, preparing);
        if (!request.quantize) return {};
        request.quantize_pure = make_texture_palette_quantizer(palette, true, preparing);
        if (!request.quantize_pure) return {};
        request.despeckle_passes = session.options.color_despeckle;
        request.out_triangle = &palette_indices;
    }
    result.mesh = build_texture_displacement(session.source->its, session.layers, session.masks, session.options,
        [&progress](int percent) { return !progress || progress(5 + percent * 95 / 100); },
        wants_color ? &request : nullptr, session.volume_to_world, nullptr, nullptr,
        session.model_uv.empty() ? nullptr : &session.model_uv);
    if (result.mesh.indices.empty()) return {};
    if (wants_color) {
        if (palette_indices.size() != result.mesh.indices.size())
            throw std::runtime_error("Color bake did not return one color per output triangle");
        result.texture_filaments.resize(palette_indices.size(), 0);
        for (size_t face = 0; face < palette_indices.size(); ++face) {
            const size_t index = palette_indices[face];
            if (index > palette.size()) throw std::runtime_error("Color bake returned an invalid palette index");
            if (index != 0) result.texture_filaments[face] = uint8_t(palette[index - 1].filament_id);
        }
    }
    if (!existing_mmu.bitstream.empty())
        result.preview_filaments = transfer_facet_paint(session.source->its, existing_mmu, result.mesh);
    result.preview_filaments.resize(result.mesh.indices.size(), 0);
    for (size_t face = 0; face < result.texture_filaments.size(); ++face)
        if (result.texture_filaments[face]) result.preview_filaments[face] = result.texture_filaments[face];
    return result;
}
} // namespace Slic3r::GUI

#include "TextureMixRecipes.inc"
