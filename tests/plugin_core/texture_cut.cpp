#include "slic3r/GUI/TextureIslandCut.hpp"
#include "slic3r/GUI/TextureEditHistory.hpp"
#include "slic3r/GUI/TextureEditingTools.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
TriangleSelector::TriangleSplittingData paint(const indexed_triangle_set& mesh, bool brush)
{
    const TriangleMesh triangles(mesh);
    TriangleSelector selector(triangles);
    if (brush) {
        selector.set_edge_limit(.3f);
        auto cursor = std::make_unique<TriangleSelector::Sphere>(Vec3f(6,3.5f,0),Vec3f(6,3.5f,10),
            2.f,Transform3d::Identity(),TriangleSelector::ClippingPlane{});
        selector.select_patch(0,std::move(cursor),EnforcerBlockerType::ENFORCER,Transform3d::Identity(),true);
    } else {
        for (size_t face = 0; face < mesh.indices.size(); ++face) selector.set_facet(int(face),EnforcerBlockerType::ENFORCER);
    }
    return selector.serialize();
}
std::vector<Vec2f> corner_uvs(const indexed_triangle_set& mesh, const TriangleSelector::TriangleSplittingData& mask,
                            const Transform3d& transform, TextureDisplacementLayer layer)
{
    auto patch = extract_painted_patch(mesh,mask,layer.lscm_seam_edges);
    const Transform3d frame = texture_displacement_bake_frame(transform);
    for (auto& vertex : patch.vertices) vertex = (frame * vertex.cast<double>()).cast<float>();
    return compute_lscm_corner_uvs(patch,layer);
}
void install_cut(TextureDisplacementLayer& layer, const TextureIslandCutResult& cut)
{
    layer.lscm_seam_edges = cut.seams; layer.islands = cut.islands; layer.island_groups = cut.groups;
    layer.lscm_uv_overrides.clear();
    for (size_t vertex = 0; vertex < cut.layout.uvs.size(); ++vertex)
        set_texture_uv_override(layer,int(vertex),cut.layout.uvs[vertex]);
}
} // namespace

void test_texture_cut(const std::filesystem::path& fixtures)
{
    // Reproduce GUI cut13: default six-chart cube, connected net disabled,
    // then rotate/scale one face before its first manually stored seam.
    indexed_triangle_set cube;
    cube.vertices = {Vec3f(0,0,0),Vec3f(10,0,0),Vec3f(10,10,0),Vec3f(0,10,0),
                     Vec3f(0,0,10),Vec3f(10,0,10),Vec3f(10,10,10),Vec3f(0,10,10)};
    cube.indices = {Vec3i32(0,2,1),Vec3i32(0,3,2),Vec3i32(4,5,6),Vec3i32(4,6,7),
                    Vec3i32(0,1,5),Vec3i32(0,5,4),Vec3i32(1,2,6),Vec3i32(1,6,5),
                    Vec3i32(2,3,7),Vec3i32(2,7,6),Vec3i32(3,0,4),Vec3i32(3,4,7)};
    const auto cube_mask = paint(cube,false);
    const auto cube_patch = extract_painted_patch(cube,cube_mask);
    auto cube_layout = compute_patch_unwrap(cube_patch,30.f,0.f);
    require(cube_layout.chart_count == 6,"Cube fixture must start with six automatic charts");
    TextureDisplacementLayer cube_layer;
    cube_layer.slot = 0; cube_layer.projection_method = TextureProjectionMethod::LSCM;
    cube_layer.lscm_seam_angle_deg = 30.f; cube_layer.auto_connect_islands = false;
    cube_layer.islands = compute_connected_net(cube_layout);
    int top = -1;
    for (size_t face=0;face<cube_layout.indices.size();++face)
        if (cube_layout.source_face[face]==2) top=cube_layout.vertex_chart[size_t(cube_layout.indices[face][0])];
    require(top >= 0,"Cube top island missing");
    cube_layer.islands[size_t(top)].rotation_deg += 15.f;
    cube_layer.islands[size_t(top)].scale *= 1.1f;
    const auto cube_before = corner_uvs(cube,cube_mask,Transform3d::Identity(),cube_layer);
    const auto cube_cut = cut_texture_island(cube,cube_mask,Transform3d::Identity(),cube_layer,
        cube_layout,cube_layer.islands,cube_layer.island_groups,top);
    require(cube_cut.layout.chart_count == 7,"Cut merged automatic cube islands instead of splitting one");
    install_cut(cube_layer,cube_cut);
    const auto cube_after = corner_uvs(cube,cube_mask,Transform3d::Identity(),cube_layer);
    require(cube_before.size() == cube_after.size(),"Cube cut changed face mapping");
    for (size_t corner=0;corner<cube_before.size();++corner)
        require((cube_before[corner]-cube_after[corner]).norm()<1e-4f,"Cube cut changed an existing island placement");
    std::map<int,int> old_face_charts;
    for (size_t face=0;face<cube_layout.indices.size();++face)
        old_face_charts[cube_layout.source_face[face]]=cube_layout.vertex_chart[size_t(cube_layout.indices[face][0])];
    std::map<int,std::set<int>> old_to_new, new_to_old;
    int bottom = -1;
    for (size_t face=0;face<cube_cut.layout.indices.size();++face) {
        const int source=cube_cut.layout.source_face[face];
        const int current=cube_cut.layout.vertex_chart[size_t(cube_cut.layout.indices[face][0])];
        old_to_new[old_face_charts.at(source)].insert(current);
        new_to_old[current].insert(old_face_charts.at(source));
        if (source==0) bottom=current;
    }
    for (const auto& [old_chart,new_charts] : old_to_new)
        require(new_charts.size()==size_t(old_chart==top?2:1),"Cut changed an unrelated cube island boundary");
    for (const auto& [new_chart,old_charts] : new_to_old)
        require(old_charts.size()==1,"Cut merged two distinct old cube charts");
    std::vector<TextureDisplacementLayer> cube_decoded; TextureDisplacementOptions cube_options;
    require(texture_displacement_layers_from_json(texture_displacement_layers_to_json({cube_layer},cube_options),cube_decoded,cube_options) &&
            corner_uvs(cube,cube_mask,Transform3d::Identity(),cube_decoded[0])==cube_after,
            "Reopening the cube recipe lost frozen automatic seams or placements");
    const auto second_cube_cut=cut_texture_island(cube,cube_mask,Transform3d::Identity(),cube_decoded[0],
        cube_cut.layout,cube_decoded[0].islands,cube_decoded[0].island_groups,bottom);
    require(second_cube_cut.layout.chart_count==8,"Repeated cube cut did not preserve the first cut topology");
    install_cut(cube_layer,second_cube_cut);
    const auto cube_twice=corner_uvs(cube,cube_mask,Transform3d::Identity(),cube_layer);
    for (size_t corner=0;corner<cube_after.size();++corner)
        require((cube_after[corner]-cube_twice[corner]).norm()<1e-4f,"Repeated cube cut moved existing UV samples");

    indexed_triangle_set mesh;
    // Unused and duplicate-position prefix vertices must never be mistaken for
    // the selected chart's original edge endpoints.
    mesh.vertices = {Vec3f::Zero(),Vec3f::Zero(),Vec3f::Zero(),Vec3f::Zero(),Vec3f::Zero(),
        Vec3f(0,0,0),Vec3f(10,0,0),Vec3f(10,10,0),Vec3f(0,10,0),
        Vec3f(30,0,0),Vec3f(40,0,0),Vec3f(40,10,0),Vec3f(30,10,0)};
    mesh.indices = {Vec3i32(5,6,7),Vec3i32(5,7,8),Vec3i32(9,10,11),Vec3i32(9,11,12)};
    const auto mask = paint(mesh,false);
    TextureDisplacementLayer layer;
    layer.slot = 0; layer.projection_method = TextureProjectionMethod::LSCM;
    layer.lscm_seam_angle_deg = 180.f; layer.auto_connect_islands = false;
    layer.lscm_seam_edges = {{9,11}};
    Transform3d transform = Transform3d::Identity();
    transform.linear().diagonal() = Vec3d(-2.,.5,1.);
    auto expanded = layer.lscm_seam_edges;
    auto patch = extract_painted_patch(mesh,mask,expanded);
    const Transform3d frame = texture_displacement_bake_frame(transform);
    for (auto& vertex : patch.vertices) vertex = (frame * vertex.cast<double>()).cast<float>();
    auto layout = compute_patch_unwrap(patch,180.f,0.f,expanded);
    require(layout.chart_count == 3,"Cut fixture did not produce the expected starting charts");
    const auto chart_of_face = [&](int source) {
        for (size_t face = 0; face < layout.indices.size(); ++face)
            if (layout.source_face[face] == source) return layout.vertex_chart[size_t(layout.indices[face][0])];
        return -1;
    };
    const int selected = chart_of_face(0), other_a = chart_of_face(2), other_b = chart_of_face(3);
    layer.islands.resize(size_t(layout.chart_count));
    for (size_t chart = 0; chart < layer.islands.size(); ++chart) {
        layer.islands[chart].offset = Vec2f(float(chart * 3 + 1),float(chart + 2));
        layer.islands[chart].scale = float(chart + 1); layer.islands[chart].rotation_deg = float(chart * 15);
    }
    join_texture_island_groups(layer.island_groups,other_a,other_b,layout.chart_count);
    set_texture_uv_override(layer,0,layout.uvs[0] + Vec2f(.3f,.2f));
    apply_lscm_uv_overrides(layout,layer.lscm_uv_overrides);
    const auto before = corner_uvs(mesh,mask,transform,layer);
    const auto original_layer = layer;
    const auto cut = cut_texture_island(mesh,mask,transform,layer,layout,layer.islands,layer.island_groups,selected);
    require(cut.layout.chart_count == 4,"Cut did not create actual new LSCM islands");
    require(std::find(cut.seams.begin(),cut.seams.end(),std::pair<int,int>{5,7}) != cut.seams.end(),
            "Cut missed the original diagonal or stored a compacted vertex index");
    for (const auto& edge : cut.seams) {
        bool actual = false;
        for (const auto& face : mesh.indices)
            for (int corner = 0; corner < 3; ++corner)
                actual |= std::minmax(face[corner],face[(corner+1)%3]) == std::minmax(edge.first,edge.second);
        require(actual,"Cut persisted a non-original mesh edge");
    }
    install_cut(layer,cut);
    const auto after = corner_uvs(mesh,mask,transform,layer);
    require(before.size() == after.size(),"Cut changed the painted face count");
    for (size_t corner = 0; corner < before.size(); ++corner)
        require((before[corner] - after[corner]).norm() < 1e-4f,"Cut renumbering lost an existing hand placement or per-copy UV edit");
    int first = -1, second = -1, group_a = -1, group_b = -1;
    for (size_t face = 0; face < cut.layout.indices.size(); ++face) {
        const int chart = cut.layout.vertex_chart[size_t(cut.layout.indices[face][0])];
        switch (cut.layout.source_face[face]) { case 0:first=chart;break; case 1:second=chart;break; case 2:group_a=chart;break; case 3:group_b=chart;break; }
    }
    require(first >= 0 && second >= 0 && first != second && texture_island_group(cut.groups,first) != texture_island_group(cut.groups,second),
            "Newly cut pieces remained joined for movement");
    require(texture_island_group(cut.groups,group_a) == texture_island_group(cut.groups,group_b),
            "Cut destroyed an unrelated island group");
    Vec2f visible_center=Vec2f::Zero(); size_t center_count=0;
    for (size_t vertex=0;vertex<cut.layout.uvs.size();++vertex)
        if (cut.layout.vertex_chart[vertex]==first) {
            visible_center+=apply_island_transform(cut.layout.uvs[vertex],first,cut.layout,cut.islands); ++center_count;
        }
    visible_center/=float(center_count);
    require((visible_center-cut.layout.chart_centroid[size_t(first)]-cut.islands[size_t(first)].offset).norm()<1e-4f,
            "Cut preserved placement but left the next rotation pivot at a stale packed location");
    layer.islands[size_t(first)].offset += Vec2f(2.f,3.f);
    const auto moved = corner_uvs(mesh,mask,transform,layer);
    for (size_t corner = 0; corner < moved.size(); ++corner) {
        Vec2f delta = Vec2f::Zero();
        if (corner < 3) delta = Vec2f(2.f,3.f);
        require((moved[corner] - after[corner] - delta).norm() < 1e-4f,"Moving one cut piece changed another piece's actual bake UVs");
    }

    Model model; auto* object = model.add_object(); object->add_instance();
    auto* volume = object->add_volume(TriangleMesh(mesh));
    std::ifstream input(fixtures / "texture-rgb.png",std::ios::binary);
    require(bool(input),"Missing cut image fixture");
    auto image = std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    TextureEditSession session(*volume); session.add_layer("Cut","memory://cut",image);
    auto old = original_layer; old.image_data = image; *session.layer(0) = old;
    TextureEditHistory history(session);
    layer.image_data = image; *session.layer(0) = layer;
    require(history.record(session,"Cut UV island") && history.undo(session) && session.layer(0)->lscm_seam_edges == old.lscm_seam_edges,
            "Undo did not restore the complete pre-cut recipe");
    require(history.redo(session) && session.layer(0)->lscm_seam_edges == layer.lscm_seam_edges,"Redo lost cut seams");
    session.apply(*volume);
    TextureEditSession reopened(*volume);
    require(corner_uvs(mesh,mask,transform,*reopened.layer(0)) == moved,"Save/reopen changed the cut piece's sampled placement");
    std::vector<TextureDisplacementLayer> decoded; TextureDisplacementOptions options;
    require(texture_displacement_layers_from_json(texture_displacement_layers_to_json(reopened.layers,reopened.options),decoded,options) &&
            corner_uvs(mesh,mask,transform,decoded[0]) == moved,"Project JSON did not preserve editable cut geometry");

    const auto brush = paint(mesh,true);
    const auto brush_patch = extract_painted_patch(mesh,brush);
    const auto brush_layout = compute_patch_unwrap(brush_patch,180.f,0.f);
    require(brush_patch.indices.size() > 2 && !brush_layout.empty(),"Cut rejection fixture did not create real brush subdivision");
    bool rejected = false;
    try { cut_texture_island(mesh,brush,Transform3d::Identity(),TextureDisplacementLayer{},brush_layout,{}, {},0); }
    catch (const std::runtime_error& e) { rejected = std::string(e.what()).find("brush-created interior edges") != std::string::npos; }
    require(rejected,"A cut through interior child edges invented persistent original-edge identity");
}
