#include "slic3r/GUI/TextureIslandLayout.hpp"
#include "slic3r/GUI/TextureEditHistory.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

bool same_placements(const std::vector<TextureIsland>& a, const std::vector<TextureIsland>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].offset != b[i].offset || a[i].rotation_deg != b[i].rotation_deg || a[i].scale != b[i].scale)
            return false;
    return true;
}
} // namespace

void test_texture_layout_tools(const std::filesystem::path& fixtures)
{
    // Two charts share a true edge; the third is disconnected, so it cannot be
    // folded into their net even when its coordinates happen to be nearby.
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0,0,0),Vec3f(10,0,0),Vec3f(10,10,0),Vec3f(0,10,0),
                     Vec3f(30,0,0),Vec3f(40,0,0),Vec3f(30,10,0)};
    mesh.indices = {Vec3i32(0,1,2),Vec3i32(0,2,3),Vec3i32(4,5,6)};
    TextureDisplacementLayer layer;
    layer.projection_method = TextureProjectionMethod::LSCM;
    layer.lscm_seam_angle_deg = 180.f;
    layer.lscm_seam_edges = {{0,2}};
    layer.auto_connect_islands = false;
    auto layout = compute_patch_unwrap(mesh,180.f,0.f,layer.lscm_seam_edges);
    require(layout.chart_count == 3, "Layout fixture did not produce three actual charts");
    layer.islands.resize(3);
    for (size_t i = 0; i < layer.islands.size(); ++i) {
        layer.islands[i].scale = float(i * 2 + 1);
        layer.islands[i].offset = Vec2f(float(i + 2),float(i + 7));
        layer.islands[i].rotation_deg = float(i * 15);
    }
    layer.island_groups = {0,0,2};
    layer.lscm_uv_overrides = {{-1,layout.uvs[0] + Vec2f(.3f,.2f)}};
    apply_lscm_uv_overrides(layout,layer.lscm_uv_overrides);
    const auto before = compute_lscm_corner_uvs(mesh,layer);
    const auto old_placements = layer.islands;
    require(average_texture_island_scale(layer.islands), "Average scale did not change unequal scales");
    const auto averaged = compute_lscm_corner_uvs(mesh,layer);
    require(averaged != before, "Average scale never reached the actual bake corner sampler");
    for (size_t i = 0; i < layer.islands.size(); ++i)
        require(layer.islands[i].scale == 3.f && layer.islands[i].offset == old_placements[i].offset &&
                layer.islands[i].rotation_deg == old_placements[i].rotation_deg,
                "Average scale changed a position/rotation or failed to use the arithmetic mean");
    for (size_t i = 0; i < layout.indices.size(); ++i) {
        const int chart = layout.vertex_chart[size_t(layout.indices[i][0])];
        const size_t corner = size_t(layout.source_face[i]) * 3;
        const float original_length = (before[corner + 1] - before[corner]).norm();
        const float new_length = (averaged[corner + 1] - averaged[corner]).norm();
        require(std::abs(new_length / original_length - 3.f / old_placements[size_t(chart)].scale) < 1e-4f,
                "Actual sampler did not apply the average scale to every chart");
    }
    require(!average_texture_island_scale(layer.islands), "Equal scales created a redundant layout action");
    const auto groups = layer.island_groups;
    const auto overrides = layer.lscm_uv_overrides;
    // Use untouched chart vertices to check exact coincidence along the shared
    // model edge. Vertex edits are restored below and verified across persistence.
    layer.lscm_uv_overrides.clear();
    layout = compute_patch_unwrap(mesh,180.f,0.f,layer.lscm_seam_edges);
    require(set_texture_islands_connected(layout,true,layer.auto_connect_islands,layer.islands),
            "Auto-connect did not apply immediately when enabled");
    const auto connected = compute_lscm_corner_uvs(mesh,layer);
    require((connected[0] - connected[3]).norm() < 1e-4f && (connected[2] - connected[4]).norm() < 1e-4f,
            "Actual bake UVs did not join both endpoints of the shared model edge");
    require(connected != averaged && layer.island_groups == groups,
            "Auto-connect made no layout or silently rewrote explicit move groups");
    const auto net = layer.islands;
    layer.islands[0].offset += Vec2f(3.f,5.f);
    const auto hand_placed = layer.islands;
    require(!set_texture_islands_connected(layout,true,layer.auto_connect_islands,layer.islands) &&
            same_placements(hand_placed,layer.islands), "Already-enabled refresh discarded a hand placement");
    require(set_texture_islands_connected(layout,false,layer.auto_connect_islands,layer.islands) &&
            same_placements(hand_placed,layer.islands), "Disabling auto-connect reset the current layout");
    require(set_texture_islands_connected(layout,true,layer.auto_connect_islands,layer.islands) &&
            same_placements(net,layer.islands), "Re-enabling auto-connect did not recompute the net");
    // Upstream deliberately retains extra stored entries through re-segmentation.
    layer.islands.push_back({}); layer.islands.back().offset = Vec2f(31.f,41.f);
    set_texture_islands_connected(layout,false,layer.auto_connect_islands,layer.islands);
    set_texture_islands_connected(layout,true,layer.auto_connect_islands,layer.islands);
    require(layer.islands.size() == 4 && layer.islands.back().offset == Vec2f(31.f,41.f),
            "Auto-connect discarded a stored placement beyond the current chart count");
    layer.lscm_uv_overrides = overrides;

    Model model; auto* object = model.add_object(); object->add_instance();
    auto* volume = object->add_volume(TriangleMesh(mesh));
    std::ifstream input(fixtures / "texture-rgb.png",std::ios::binary);
    require(bool(input), "Missing layout image fixture");
    auto bytes = std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    TextureEditSession session(*volume); session.add_layer("Layout tools","memory://layout",bytes);
    session.layer(0)->auto_connect_islands = false;
    TextureEditHistory history(session);
    auto image = session.layer(0)->image_data;
    layer.slot = 0; layer.image_data = image;
    *session.layer(0) = layer;
    require(history.record(session,"UV layout tools"), "UV layout action was not recorded");
    require(history.undo(session) && !session.layer(0)->auto_connect_islands && session.layer(0)->islands.empty(),
            "Undo did not restore the auto-connect flag and original placement together");
    require(history.redo(session) && session.layer(0)->auto_connect_islands && same_placements(session.layer(0)->islands,layer.islands),
            "Redo did not restore the auto-connect flag and placement together");
    session.apply(*volume);
    TextureEditSession reopened(*volume);
    require(reopened.layer(0)->auto_connect_islands && reopened.layer(0)->island_groups == groups &&
            reopened.layer(0)->lscm_uv_overrides == overrides && same_placements(reopened.layer(0)->islands,layer.islands),
            "Save/reopen lost layout controls, groups or vertex edits");
    std::vector<TextureDisplacementLayer> decoded;
    TextureDisplacementOptions options;
    require(texture_displacement_layers_from_json(texture_displacement_layers_to_json(reopened.layers,reopened.options),decoded,options),
            "Project JSON did not reload the edited layout");
    require(decoded[0].auto_connect_islands && compute_lscm_corner_uvs(mesh,decoded[0]) == compute_lscm_corner_uvs(mesh,layer),
            "Project persistence changed the actual bake UVs of the edited layout");
}
