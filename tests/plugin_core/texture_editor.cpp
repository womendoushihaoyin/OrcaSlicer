#include "slic3r/GUI/TextureEditSession.hpp"
#include "libslic3r/TextureDisplacementUV.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

using namespace Slic3r;
using Slic3r::GUI::TextureEditSession;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action, const char* message)
{
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
std::shared_ptr<std::vector<unsigned char>> image(const std::filesystem::path& fixtures)
{
    std::ifstream input(fixtures / "texture-white.png", std::ios::binary);
    require(bool(input), "Missing texture editor fixture");
    return std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
TriangleSelector::TriangleSplittingData paint(const TriangleMesh& mesh, std::initializer_list<int> faces)
{
    TriangleSelector selector(mesh);
    for (int face : faces) selector.set_facet(face, EnforcerBlockerType::ENFORCER);
    return selector.serialize();
}
std::vector<Vec2f> uv_channel(const TriangleMesh& mesh)
{
    std::vector<Vec2f> uv;
    for (const auto& face : mesh.its.indices)
        for (int corner = 0; corner < 3; ++corner)
            uv.push_back(mesh.its.vertices[size_t(face[corner])].head<2>() / 8.f);
    return uv;
}
void set_other_paint(ModelVolume& volume)
{
    const std::array<FacetsAnnotation*, 4> channels{
        &volume.supported_facets, &volume.seam_facets, &volume.mmu_segmentation_facets, &volume.fuzzy_skin_facets};
    for (size_t channel = 0; channel < channels.size(); ++channel) {
        TriangleSelector selector(volume.mesh());
        selector.set_facet(int(channel), EnforcerBlockerType::ENFORCER);
        channels[channel]->set(selector);
    }
}

void test_classic_result_options(const std::filesystem::path& fixtures)
{
    indexed_triangle_set grid;
    const auto vertex = [](int x, int y) { return y * 9 + x; };
    for (int y = 0; y < 9; ++y)
        for (int x = 0; x < 9; ++x) grid.vertices.emplace_back(float(x), float(y), 1.f);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            grid.indices.emplace_back(vertex(x,y), vertex(x+1,y), vertex(x+1,y+1));
            grid.indices.emplace_back(vertex(x,y), vertex(x+1,y+1), vertex(x,y+1));
        }
    const TriangleMesh source(grid);
    TriangleSelector selector(source);
    for (int y = 2; y < 6; ++y)
        for (int x = 2; x < 6; ++x) {
            selector.set_facet(2 * (y * 8 + x), EnforcerBlockerType::ENFORCER);
            selector.set_facet(2 * (y * 8 + x) + 1, EnforcerBlockerType::ENFORCER);
        }
    TextureDisplacementLayer layer;
    layer.slot = 0; layer.path = "classic-result.png"; layer.image_data = image(fixtures);
    layer.projection_method = TextureProjectionMethod::Triplanar; layer.depth_mm = .4f;
    std::array<TriangleSelector::TriangleSplittingData, TEXTURE_DISPLACEMENT_MAX_LAYERS> masks;
    masks[0] = selector.serialize();
    TextureDisplacementOptions options;
    options.pipeline_v2 = false;
    const auto bake = [&] { return build_texture_displacement(grid, {layer}, masks, options); };
    const auto free_border = bake();
    options.displace_border = false;
    const auto pinned = bake();
    require(free_border.indices == grid.indices && pinned.indices == grid.indices,
            "Classic result controls unexpectedly rebuilt topology");
    require(free_border.vertices[size_t(vertex(2,4))].z() > 1.39f &&
            pinned.vertices[size_t(vertex(2,4))].z() == 1.f &&
            pinned.vertices[size_t(vertex(4,4))].z() > 1.39f,
            "Classic border option did not distinguish the paint rim from its interior");
    options.smooth_enabled = true; options.smooth_strength = .4f; options.smooth_iterations = 3;
    const auto smoothed = bake();
    require(smoothed.vertices[size_t(vertex(4,4))].z() < pinned.vertices[size_t(vertex(4,4))].z() - .001f,
            "Geometry smoothing did not change the actual displaced interior");
    require(smoothed.vertices[size_t(vertex(2,4))] == pinned.vertices[size_t(vertex(2,4))] &&
            smoothed.vertices[size_t(vertex(0,4))] == grid.vertices[size_t(vertex(0,4))],
            "Smoothing changed the protected outer ring or an unpainted vertex");
    options.displace_border = true;
    const auto protected_rim = bake();
    options.smooth_skip_border = false;
    const auto relaxed_rim = bake();
    require(protected_rim.vertices[size_t(vertex(2,4))].z() > 1.39f &&
            relaxed_rim.vertices[size_t(vertex(2,4))].z() < protected_rim.vertices[size_t(vertex(2,4))].z() - .001f,
            "Ignore outer ring did not control the real smoothing boundary");
    options.smooth_iterations = 0;
    require(bake().vertices == free_border.vertices, "Zero smoothing passes still changed geometry");
}
}

void test_texture_editor(const std::string& scenario, const std::filesystem::path& fixtures)
{
    if (scenario == "texture_editor_result") { test_classic_result_options(fixtures); return; }
    Model model;
    auto* object = model.add_object();
    object->add_instance();
    auto* volume = object->add_volume(make_cube(8,8,8));
    volume->texture_uv = uv_channel(volume->mesh());
    set_other_paint(*volume);
    const auto source_mesh = volume->mesh_ptr();
    const auto source_id = volume->id();
    TextureEditSession session(*volume);
    const auto bytes = image(fixtures);
    session.add_layer("first", "plugin://texture/white", bytes);
    session.layer(0)->depth_mm = .37f;
    session.layer(0)->offset = Vec2f(.25f, -.5f);
    session.layer(0)->view_project_right = Vec3f(0,1,0);
    session.masks[0] = paint(volume->mesh(), {0,1});
    require(session.layer(0)->projection_method == TextureProjectionMethod::ModelUV, "New layer ignored the model's UV channel");
    require(volume->texture_displacement_layers.empty() && volume->texture_displacement_facet(0).empty(),
            "Editing a session changed the live model before Apply");

    if (scenario == "texture_editor") {
        for (int slot = 1; slot < 8; ++slot) {
            require(session.add_layer("layer " + std::to_string(slot), "", bytes) == slot, "Layer slots were not allocated safely");
            session.masks[size_t(slot)] = paint(volume->mesh(), {slot});
        }
        rejects([&] { session.add_layer("ninth", "", bytes); }, "Editor accepted a ninth layer");
        const auto first_mask = session.masks[0], second_mask = session.masks[1];
        require(session.move_layer(0,1), "Layer reorder failed");
        require(session.layer(1)->name == "first" && session.masks[1] == first_mask && session.masks[0] == second_mask &&
                session.options.active_slot == 1, "Layer reorder detached its image, paint or active slot");
        require(session.remove_layer(0) && !TriangleSelector::has_facets(session.masks[0], EnforcerBlockerType::ENFORCER),
                "Removing a layer left its paint in the reusable slot");
        require(session.add_layer("replacement", "", bytes) == 0 && session.masks[0].bitstream.empty(),
                "Reused slot inherited a deleted layer's mask");
        session.options.pipeline_v2 = false;
        session.options.v2_max_triangles_k = -1;
        session.options.smooth_enabled = true;
        const auto support = volume->supported_facets.get_data(), seam = volume->seam_facets.get_data();
        const auto mmu = volume->mmu_segmentation_facets.get_data(), fuzzy = volume->fuzzy_skin_facets.get_data();
        session.apply(*volume);
        require(volume->mesh_ptr() == source_mesh && volume->id() == source_id, "Saving editable layers changed geometry identity");
        require(volume->supported_facets.get_data() == support && volume->seam_facets.get_data() == seam &&
                volume->mmu_segmentation_facets.get_data() == mmu && volume->fuzzy_skin_facets.get_data() == fuzzy,
                "Saving editable layers altered another painting tool's data");
        TextureEditSession reopened(*volume);
        require(reopened.layers.size() == 8 && reopened.layer(1)->name == "first" &&
                reopened.layer(1)->image_data && *reopened.layer(1)->image_data == *bytes &&
                reopened.layer(1)->offset == Vec2f(.25f,-.5f) && reopened.masks == session.masks &&
                reopened.model_uv == session.model_uv && reopened.options.v2_max_triangles_k == -1 &&
                !reopened.options.pipeline_v2 && reopened.options.smooth_enabled,
                "Reopening the editor lost layer parameters, image data, masks, UV or automatic settings");
        const auto json = texture_displacement_layers_to_json(reopened.layers, reopened.options);
        std::vector<TextureDisplacementLayer> restored;
        TextureDisplacementOptions options;
        require(texture_displacement_layers_from_json(json, restored, options) && restored.size() == 8 &&
                restored[1].offset == Vec2f(.25f,-.5f) && options.v2_max_triangles_k == -1,
                "Editor metadata cannot roundtrip through the project serializer");
        auto* second_instance = object->add_instance();
        second_instance->set_scaling_factor(Vec3d(2.,3.,4.));
        second_instance->set_mirror(Vec3d(-1.,1.,1.));
        TextureEditSession second(*volume, 1);
        require(second.volume_to_world.matrix().isApprox((second_instance->get_matrix() * volume->get_matrix()).matrix()),
                "Editor used the first instance's transform instead of the selected instance");
        second_instance->set_scaling_factor(Vec3d(2.,3.,5.));
        require(!second.source_is_current(*volume), "Editor failed to reject a changed selected instance");
        return;
    }
    if (scenario == "texture_editor_invalid") {
        auto malformed = session;
        malformed.model_uv[0].x() = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { malformed.apply(*volume); }, "Non-finite UVs reached the live model");
        malformed = session;
        malformed.masks[0].bitstream.pop_back();
        rejects([&] { malformed.apply(*volume); }, "Truncated mask was deserialized unsafely");
        malformed = session;
        malformed.layer(0)->projection_method = TextureProjectionMethod(99);
        rejects([&] { malformed.apply(*volume); }, "Unknown projection mode reached a UI index");
        rejects([&] { session.apply(*volume, volume->mesh().its); }, "No-op bake consumed editable masks");
        auto bad_mesh = volume->mesh().its;
        bad_mesh.indices[0][0] = int(bad_mesh.vertices.size());
        rejects([&] { session.apply(*volume, bad_mesh); }, "Invalid baked index reached paint transfer");
        malformed = session;
        malformed.layer(0)->color_enabled = true;
        rejects([&] { malformed.apply(*volume, volume->mesh().its); }, "Geometry-only bake silently consumed a color layer");
        const auto transform = volume->get_matrix();
        auto shifted = transform; shifted.translation().x() += 1.; volume->set_transformation(shifted);
        rejects([&] { session.apply(*volume); }, "A stale transformed source accepted pending texture edits");
        volume->set_transformation(transform);
        require(volume->mesh_ptr() == source_mesh && volume->id() == source_id && volume->texture_displacement_layers.empty(),
                "A rejected edit partially published geometry or layer data");
        return;
    }
    require(scenario == "texture_editor_bake", "Unknown texture editor scenario");
    session.layer(0)->projection_method = TextureProjectionMethod::Triplanar;
    session.options.v2_refine_mm = 2.f;
    session.options.v2_max_triangles_k = 50;
    TriangleSelector all(volume->mesh());
    for (size_t face = 0; face < volume->mesh().its.indices.size(); ++face)
        all.set_facet(int(face), EnforcerBlockerType::ENFORCER);
    session.masks[0] = all.serialize();
    session.add_layer("missing image, unconsumed", "unavailable.png", bytes);
    session.layer(1)->image_data.reset();
    session.masks[1] = paint(volume->mesh(), {8,9});
    auto baked = build_texture_displacement(session.source->its, session.layers, session.masks, session.options, {}, nullptr,
                                           session.volume_to_world, nullptr, nullptr, &session.model_uv);
    require(baked.indices.size() > source_mesh->its.indices.size(), "Editor bake did not run the full refinement kernel");
    const std::array<const FacetsAnnotation*, 4> original{
        &volume->supported_facets, &volume->seam_facets, &volume->mmu_segmentation_facets, &volume->fuzzy_skin_facets};
    std::array<std::vector<uint8_t>, 4> expected;
    for (size_t channel = 0; channel < original.size(); ++channel)
        expected[channel] = transfer_facet_paint(source_mesh->its, original[channel]->get_data(), baked);
    const auto expected_survivor = transfer_facet_paint(source_mesh->its, session.masks[1], baked);
    session.apply(*volume, std::move(baked));
    require(volume->mesh_ptr() != source_mesh && volume->id() != source_id, "Explicit Bake did not replace geometry identity");
    require(volume->texture_displacement_layers.size() == 2 && volume->texture_displacement_layers[0].image_data == bytes &&
            volume->texture_displacement_layers[0].depth_mm == .37f && volume->texture_displacement_layers[0].offset == Vec2f(.25f,-.5f),
            "Bake discarded the editable texture images or parameters");
    for (int slot = 0; slot < 8; ++slot)
        if (slot != 1) require(volume->texture_displacement_facet(slot).empty(), "Bake left consumed masks on new topology");
    TriangleSelector surviving(volume->mesh()); surviving.deserialize(volume->texture_displacement_facet(1).get_data());
    require(!volume->texture_displacement_facet(1).empty() &&
            surviving.get_facets_strict(EnforcerBlockerType::ENFORCER).indices.size() ==
                size_t(std::count(expected_survivor.begin(), expected_survivor.end(), uint8_t(1))),
            "Bake discarded a missing-image layer's unconsumed mask instead of transferring it");
    require(volume->texture_uv.size() == volume->mesh().its.indices.size() * 3, "Bake lost the model's per-corner UV channel");
    const std::array<const FacetsAnnotation*, 4> actual{
        &volume->supported_facets, &volume->seam_facets, &volume->mmu_segmentation_facets, &volume->fuzzy_skin_facets};
    for (size_t channel = 0; channel < actual.size(); ++channel) {
        TriangleSelector selector(volume->mesh()); selector.deserialize(actual[channel]->get_data());
        const auto selected = selector.get_facets_strict(EnforcerBlockerType::ENFORCER);
        require(selected.indices.size() == size_t(std::count(expected[channel].begin(), expected[channel].end(), uint8_t(1))),
                "Bake failed to transfer an existing paint channel to the new topology");
    }
    TextureEditSession reopened(*volume);
    require(reopened.layers.size() == 2 && reopened.model_uv.size() == volume->texture_uv.size(),
            "The baked model could not be reopened for another editable layer");
}
