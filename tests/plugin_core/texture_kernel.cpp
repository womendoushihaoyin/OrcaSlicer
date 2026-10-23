#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TextureDisplacementParameterize.hpp"
#include "libslic3r/Model.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace Slic3r;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }
TextureDisplacementLayer layer(const std::filesystem::path& fixtures, int slot, const char* file = "texture-white.png")
{
    std::ifstream input(fixtures / file, std::ios::binary);
    require(bool(input), "Missing texture kernel fixture");
    TextureDisplacementLayer result;
    result.slot = slot;
    result.image_data = std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return result;
}
indexed_triangle_set plane()
{
    indexed_triangle_set result;
    result.vertices = {Vec3f(0, 0, 5), Vec3f(2, 0, 5), Vec3f(2, 2, 5), Vec3f(0, 2, 5)};
    result.indices = {Vec3i32(0, 1, 2), Vec3i32(0, 2, 3)};
    return result;
}
TriangleSelector::TriangleSplittingData paint(const indexed_triangle_set& mesh, std::initializer_list<int> faces)
{
    const TriangleMesh triangles(mesh);
    TriangleSelector selector(triangles);
    for (int face : faces) selector.set_facet(face, EnforcerBlockerType::ENFORCER);
    return selector.serialize();
}
void check_finite(const indexed_triangle_set& mesh)
{
    require(!mesh.indices.empty(), "Texture kernel returned no geometry");
    for (const Vec3f& vertex : mesh.vertices) require(vertex.allFinite(), "Texture kernel returned non-finite geometry");
    for (const auto& face : mesh.indices)
        for (int corner = 0; corner < 3; ++corner)
            require(face[corner] >= 0 && size_t(face[corner]) < mesh.vertices.size(), "Texture kernel returned invalid indices");
}
}

void test_texture_kernel(const std::string& scenario, const std::filesystem::path& fixtures)
{
    const auto mesh = plane();
    auto base = layer(fixtures, 0);
    TextureDisplacementFacetsData masks;
    masks[0] = paint(mesh, {0, 1});
    if (scenario == "texture_projection") {
        auto top = layer(fixtures, 7);
        top.depth_mm = .2f;
        top.blend_mode = TextureBlendMode::Override;
        masks[7] = paint(mesh, {1});
        const auto sampler = make_combined_displacement_sampler(mesh, {top, base}, masks);
        require(bool(sampler), "Layer sampler is empty");
        require(near(sampler(Vec3f(1.5f, .5f, 5), Vec3f::UnitZ()), .4f), "Upper layer leaked outside its painted patch");
        require(near(sampler(Vec3f(.5f, 1.5f, 5), Vec3f::UnitZ()), .2f), "Slot order or Override blend changed");
        for (const auto& [mode, expected] : std::array<std::pair<TextureBlendMode, float>, 5>{{
                 {TextureBlendMode::Add, .6f}, {TextureBlendMode::Subtract, .2f},
                 {TextureBlendMode::Multiply, .08f}, {TextureBlendMode::Divide, 2.f}, {TextureBlendMode::Override, .2f}}}) {
            top.blend_mode = mode;
            const auto blended = make_combined_displacement_sampler(mesh, {top, base}, masks);
            require(near(blended(Vec3f(.5f, 1.5f, 5), Vec3f::UnitZ()), expected), "Painted layer blend produced the wrong displacement");
        }

        auto uv_layer = layer(fixtures, 0, "texture-gray.png");
        uv_layer.projection_method = TextureProjectionMethod::ModelUV;
        uv_layer.depth_mm = 1.f;
        uv_layer.tiling_scale = 999.f; // Imported UVs are normalized, never millimetres.
        std::vector<Vec2f> uv(6, Vec2f::Zero());
        std::fill(uv.begin() + 3, uv.end(), Vec2f(.5f, .5f));
        const auto uv_sampler = make_combined_displacement_sampler(mesh, {uv_layer}, masks, &uv);
        require(near(uv_sampler(Vec3f(1.5f, .5f, 5), Vec3f::UnitZ()), 0.f) &&
                near(uv_sampler(Vec3f(.5f, 1.5f, 5), Vec3f::UnitZ()), 1.f), "Model UV seams or normalized scale lost in the actual sampler");
        uv_layer.image_data.reset();
        uv.clear();
        require(near(uv_sampler(Vec3f(.5f, 1.5f, 5), Vec3f::UnitZ()), 1.f), "Prepared sampler retained borrowed image or UV data");

        const auto white = decode_height_texture(base);
        require(!white.empty(), "Layer decoder failed");
        for (auto method : {TextureProjectionMethod::Triplanar, TextureProjectionMethod::Cylindrical,
                            TextureProjectionMethod::Spherical, TextureProjectionMethod::ViewProjected,
                            TextureProjectionMethod::LSCM, TextureProjectionMethod::ModelUV}) {
            base.projection_method = method;
            require(near(sample_layer_height(white, base, Vec3f(.5f, .5f, 5), Vec3f::UnitZ()), 1.f), "Projection failed to sample a uniform texture");
        }
        base.projection_method = TextureProjectionMethod::ViewProjected;
        base.view_project_projective = true;
        base.view_project_matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        require(near(sample_layer_height(white, base, Vec3f(.5f, .5f, -1), Vec3f::UnitZ()), 0.f), "Projector sampled behind its camera plane");

        auto placed = layer(fixtures, 0, "texture-gray.png");
        placed.tiling_scale = 1.f;
        const auto gray = decode_height_texture(placed);
        require(near(sample_layer_height(gray, placed, Vec3f(.5f,0,0), Vec3f::UnitZ()), 64.f/255.f), "Triplanar projection sampled the wrong pixel");
        require(near(sample_layer_height(gray, placed, Vec3f(.5f,0,0), Vec3f(1,0,1).normalized()), 32.f/255.f), "Triplanar normal weighting was not blended");
        placed.projection_method = TextureProjectionMethod::Cylindrical;
        require(near(sample_layer_height(gray, placed, Vec3f(0,1,-.5f), Vec3f::UnitY()), 192.f/255.f), "Cylindrical axial projection sampled the wrong pixel");
        placed.projection_method = TextureProjectionMethod::Spherical;
        require(near(sample_layer_height(gray, placed, Vec3f(0,float(1.0/M_PI),0), Vec3f::UnitY()), 64.f/255.f), "Spherical longitude projection sampled the wrong pixel");
        placed.projection_method = TextureProjectionMethod::ViewProjected;
        require(near(sample_layer_height(gray, placed, Vec3f(.5f,0,0), Vec3f::UnitZ()), 64.f/255.f), "View projection sampled the wrong pixel");
        const Vec2f known_uv(.5f,0);
        placed.projection_method = TextureProjectionMethod::LSCM;
        require(near(sample_layer_height(gray, placed, Vec3f::Zero(), Vec3f::UnitZ(), Vec3f::Zero(), Vec3f::UnitZ(), &known_uv), 64.f/255.f), "Unwrapped UV did not reach the texture sampler");
        auto rgb = layer(fixtures, 0, "texture-rgb.png");
        rgb.color_enabled = true;
        rgb.projection_method = TextureProjectionMethod::ModelUV;
        std::vector<Vec2f> rgb_uv(6, Vec2f::Zero());
        const auto colors = make_combined_color_sampler(mesh, {rgb}, masks,
            [](const Vec3f& color) { return color.x() > .9f && color.y() < .1f ? 4 : -1; }, {}, &rgb_uv);
        require(colors && colors(Vec3f(1.5f, .5f, 5), Vec3f::UnitZ()) == 4, "Texture colour did not follow Model UV placement");
        TextureColorRequest color_request;
        color_request.quantize = [](const Vec3f& color) { return color.x() > .9f && color.y() < .1f ? 4 : -1; };
        color_request.min_color_region_mm2 = 0;
        std::vector<uint8_t> face_colors;
        color_request.out_triangle = &face_colors;
        TextureDisplacementOptions color_options;
        color_options.pipeline_v2 = false;
        const auto colored = build_texture_displacement(mesh, {rgb}, masks, color_options, {}, &color_request,
            Transform3d::Identity(), nullptr, nullptr, &rgb_uv);
        require(face_colors.size() == colored.indices.size() && !face_colors.empty() &&
                std::all_of(face_colors.begin(), face_colors.end(), [](uint8_t value) { return value == 5; }),
                "Texture bake did not return per-triangle palette assignments");
        return;
    }
    if (scenario == "texture_lscm") {
        indexed_triangle_set disk;
        disk.vertices = {Vec3f(-1,-1,0), Vec3f(1,-1,0), Vec3f(1,1,0), Vec3f(-1,1,0), Vec3f(0,0,.3f)};
        disk.indices = {Vec3i32(0,1,4), Vec3i32(1,2,4), Vec3i32(2,3,4), Vec3i32(3,0,4)};
        const auto mapped = parameterize_texture_lscm(disk);
        require(mapped && mapped->size() == disk.vertices.size(), "CGAL LSCM did not parameterize a curved disk");
        float area = 0.f;
        for (const auto& face : disk.indices) {
            const Vec2f a = (*mapped)[face[1]] - (*mapped)[face[0]], b = (*mapped)[face[2]] - (*mapped)[face[0]];
            require(a.allFinite() && b.allFinite(), "LSCM produced non-finite UVs");
            area += std::abs(a.x() * b.y() - a.y() * b.x());
        }
        require(area > 1e-4f, "LSCM collapsed the curved chart");
        require(!parameterize_texture_lscm(its_make_cube(2,2,2)), "LSCM accepted a closed surface as one disk");
        const auto unwrap = compute_patch_unwrap(its_make_cube(2,2,2));
        require(unwrap.chart_count == 6 && unwrap.source_face.size() == 12, "Cube unwrap did not retain six planar charts and face correspondence");
        const auto net = compute_connected_net(unwrap);
        require(net.size() == 6, "Connected UV net lost an island");
        base.projection_method = TextureProjectionMethod::LSCM;
        const auto corners = compute_lscm_corner_uvs(disk, base);
        require(corners.size() == disk.indices.size() * 3, "LSCM per-corner channel missing");

        // Real brush subdivision and T-joint export: the source face must be exact even when
        // neighbouring triangles share an edge or are split to different depths.
        const TriangleMesh triangles(mesh);
        TriangleSelector selector(triangles);
        selector.set_edge_limit(.15f);
        auto cursor = std::make_unique<TriangleSelector::Sphere>(Vec3f(1.2f,.7f,5), Vec3f(1.2f,.7f,10),
            .45f, Transform3d::Identity(), TriangleSelector::ClippingPlane{});
        selector.select_patch(0, std::move(cursor), EnforcerBlockerType::ENFORCER, Transform3d::Identity(), true);
        std::vector<int> source;
        const auto pieces = selector.get_facets_strict(EnforcerBlockerType::ENFORCER, &source);
        require(pieces.indices.size() > 2 && source.size() == pieces.indices.size(), "Brush did not export exact sub-triangle provenance");
        std::vector<Vec2f> uv(6, Vec2f::Zero());
        std::fill(uv.begin() + 3, uv.end(), Vec2f(1,1));
        const auto piece_uv = compute_model_corner_uvs(mesh, uv, pieces, source);
        for (size_t face = 0; face < source.size(); ++face) {
            require(source[face] == 0 || source[face] == 1, "Invalid original face from painted subdivision");
            for (size_t corner = 0; corner < 3; ++corner)
                require((piece_uv[face*3+corner] - Vec2f(float(source[face]),float(source[face]))).norm() < 1e-5f,
                        "Brush subdivision crossed a per-corner UV seam");
        }
        return;
    }
    require(scenario == "texture_kernel", "Unknown editable texture kernel scenario");
    TextureDisplacementOptions options;
    options.pipeline_v2 = false;
    auto classic = build_texture_displacement(mesh, {base}, masks, options);
    check_finite(classic);
    for (const auto& vertex : classic.vertices) require(near(vertex.z(), 5.4f), "Classic editable layer did not move the actual mesh");
    Transform3d scaled = Transform3d::Identity();
    scaled.linear() = Vec3d(2,3,4).asDiagonal();
    scaled.translation() = Vec3d(100,200,300);
    const auto physical = build_texture_displacement(mesh, {base}, masks, options, {}, nullptr, scaled);
    for (const auto& vertex : physical.vertices) require(near(vertex.z(), 5.1f), "Texture depth was scaled instead of remaining world millimetres");
    auto canceled = build_texture_displacement(mesh, {base}, masks, options, [](int) { return false; });
    require(canceled.indices.empty(), "Canceled editable bake published a mesh");

    Model model;
    auto* object = model.add_object();
    object->add_instance();
    auto* volume = object->add_volume(make_cube(8,8,8));
    volume->texture_displacement_layers = {base};
    TriangleSelector selector(volume->mesh());
    for (size_t face = 0; face < volume->mesh().its.indices.size(); ++face)
        selector.set_facet(int(face), EnforcerBlockerType::ENFORCER);
    volume->texture_displacement_facet(0).set(selector);
    volume->texture_displacement_options.v2_refine_mm = 2.f;
    volume->texture_displacement_options.v2_max_triangles_k = 50;
    const auto baked = build_texture_displacement(*volume);
    check_finite(baked);
    require(baked.indices.size() > volume->mesh().its.indices.size(), "ModelVolume did not reach the full refinement pipeline");
    require(TriangleMesh(baked).volume() > 512., "Editable ModelVolume bake did not displace volume");
    require(its_num_open_edges(baked) == 0, "Editable layer pipeline introduced open edges");
    require(volume->texture_displacement_layers.size() == 1 && volume->mesh().its.indices.size() == 12,
            "Kernel mutated the live editable source model");
}
