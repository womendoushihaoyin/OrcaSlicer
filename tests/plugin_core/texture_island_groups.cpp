#include "slic3r/GUI/TextureIslandGroups.hpp"
#include "slic3r/GUI/TextureEditHistory.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}

void test_texture_island_groups(const std::filesystem::path& fixtures)
{
    std::vector<int> selected;
    int primary = -1;
    require(select_texture_island(selected,primary,0,false,false) && selected == std::vector<int>{0}, "Plain island selection failed");
    require(select_texture_island(selected,primary,1,true,false) && selected == std::vector<int>({0,1}), "Shift did not add an island");
    require(select_texture_island(selected,primary,0,false,false) && selected.size() == 2 && primary == 0,
            "Plain drag on a selected island discarded the multi-selection");
    require(!select_texture_island(selected,primary,0,false,true) && selected == std::vector<int>{1} && primary == 1,
            "Ctrl toggle did not deselect the clicked island");
    select_texture_island(selected,primary,-1,false,false);
    require(selected.empty() && primary == -1, "Empty-space click did not clear island selection");

    std::vector<int> groups;
    join_texture_island_groups(groups,0,1,4);
    join_texture_island_groups(groups,1,2,4);
    require(texture_island_move_set(groups,4,{2},2) == std::vector<int>({0,1,2}), "Joined group did not expand the move set");
    unjoin_texture_island_group(groups,0,4);
    require(texture_island_move_set(groups,4,{0},0) == std::vector<int>{0} &&
            texture_island_move_set(groups,4,{1},1) == std::vector<int>({1,2}),
            "Unjoining the representative failed to detach it or split the remaining group");
    require(texture_island_move_set(groups,4,{0,2},2) == std::vector<int>({0,1,2}), "Multi-selection did not include both joined groups");

    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0,0,0),Vec3f(10,0,0),Vec3f(10,10,0),Vec3f(0,10,0),
                     Vec3f(30,0,0),Vec3f(40,0,0),Vec3f(30,10,0)};
    mesh.indices = {Vec3i32(0,1,2),Vec3i32(0,2,3),Vec3i32(4,5,6)};
    TextureDisplacementLayer layer;
    layer.projection_method = TextureProjectionMethod::LSCM;
    layer.lscm_seam_angle_deg = 180.f;
    layer.lscm_seam_edges = {{0,2}};
    layer.auto_connect_islands = false;
    const auto layout = compute_patch_unwrap(mesh,180.f,0.f,layer.lscm_seam_edges);
    require(layout.chart_count == 3, "Group fixture did not produce three real charts");
    const auto chart_of_face = [&](int face) {
        for (size_t i=0;i<layout.source_face.size();++i)
            if (layout.source_face[i] == face) return layout.vertex_chart[size_t(layout.indices[i][0])];
        return -1;
    };
    const int first = chart_of_face(0), second = chart_of_face(1), detached = chart_of_face(2);
    require(first >= 0 && second >= 0 && detached >= 0, "Chart source-face mapping missing");
    layer.islands.resize(size_t(layout.chart_count));
    require(join_nearest_texture_island(layout,first,layer.islands,layer.island_groups), "Join failed for actual edge-sharing charts");
    require(!join_nearest_texture_island(layout,detached,layer.islands,layer.island_groups), "Join invented an edge to a disconnected island");
    for (const int source_vertex : {0,2}) {
        Vec2f a, b; bool found_a=false, found_b=false;
        for (size_t i=0;i<layout.source_vertex.size();++i) {
            if (layout.source_vertex[i] != source_vertex) continue;
            const int chart = layout.vertex_chart[i];
            if (chart == first) { a=apply_island_transform(layout.uvs[i],chart,layout,layer.islands); found_a=true; }
            if (chart == second) { b=apply_island_transform(layout.uvs[i],chart,layout,layer.islands); found_b=true; }
        }
        require(found_a && found_b && (a-b).norm()<1e-4f, "Joined charts did not align their shared edge endpoints");
    }
    const auto before = compute_lscm_corner_uvs(mesh,layer);
    const Vec2f delta(2.f,3.f);
    for (const int chart : texture_island_move_set(layer.island_groups,layout.chart_count,{first},first))
        layer.islands[size_t(chart)].offset += delta;
    const auto after = compute_lscm_corner_uvs(mesh,layer);
    require(before.size() == after.size() && after.size() == mesh.indices.size()*3, "Actual bake corner sampler missing UVs");
    for (size_t corner=0;corner<after.size();++corner) {
        Vec2f expected = Vec2f::Zero();
        if (corner < 6) expected = delta;
        require((after[corner]-before[corner]-expected).norm()<1e-4f,
                "Group translation did not reach actual bake UVs or moved an unjoined island");
    }

    Model model; auto* object=model.add_object(); object->add_instance();
    auto* volume=object->add_volume(TriangleMesh(mesh));
    std::ifstream input(fixtures/"texture-rgb.png",std::ios::binary);
    require(bool(input),"Missing island image fixture");
    auto bytes=std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    TextureEditSession session(*volume); session.add_layer("Island group","memory://groups",bytes);
    TextureEditHistory history(session);
    auto& stored=*session.layer(0);
    stored.projection_method=layer.projection_method; stored.lscm_seam_angle_deg=layer.lscm_seam_angle_deg;
    stored.auto_connect_islands=layer.auto_connect_islands; stored.lscm_seam_edges=layer.lscm_seam_edges; stored.islands=layer.islands; stored.island_groups=layer.island_groups;
    require(history.record(session,"Join UV islands"),"Joined layout was not recorded in editor history");
    require(history.undo(session) && session.layer(0)->island_groups.empty(),"Undo did not restore the ungrouped layer");
    require(history.redo(session) && session.layer(0)->island_groups==layer.island_groups,"Redo did not restore the joined layer");
    session.apply(*volume);
    TextureEditSession reopened(*volume);
    require(reopened.layer(0)->island_groups==layer.island_groups &&
            texture_island_move_set(reopened.layer(0)->island_groups,layout.chart_count,{first},first).size()==2,
            "Save/reopen did not retain a usable joined move group");
    require(compute_lscm_corner_uvs(mesh,*reopened.layer(0))==after,"Saved island placement changed the actual UV sampler");
}
