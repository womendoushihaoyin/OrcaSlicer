#include "slic3r/GUI/TextureEditingTools.hpp"
#include "slic3r/GUI/TextureUVSelection.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
}
void test_texture_authoring(const std::filesystem::path& fixtures)
{
    indexed_triangle_set mesh;
    mesh.vertices={Vec3f(0,0,0),Vec3f(10,0,0),Vec3f(10,10,0),Vec3f(0,10,0),Vec3f(50,50,50)};
    mesh.indices={Vec3i32(0,1,2),Vec3i32(0,2,3)};
    TextureDisplacementLayer layer;
    require(toggle_texture_seam(layer,2,0) && layer.lscm_seam_edges==std::vector<std::pair<int,int>>{{0,2}},
            "Manual seam was not canonicalized in original mesh index space");
    require(toggle_texture_seam(layer,0,2) && layer.lscm_seam_edges.empty(), "Clicking a marked seam did not clear it");
    const auto path=texture_seam_path(mesh,1,3);
    require(path.size()==2 && texture_seam_path(mesh,1,4).empty(), "Seam path crossed a disconnected component or missed shortest edges");
    for (const auto& edge:path) {
        bool exists=false;
        for (const auto& face:mesh.indices)
            for (int c=0;c<3;++c)
                exists |= (face[c]==edge.first && face[(c+1)%3]==edge.second) ||
                          (face[c]==edge.second && face[(c+1)%3]==edge.first);
        require(exists,"Seam path invented a non-mesh edge");
    }
    layer.lscm_seam_angle_deg=180.f; layer.lscm_seam_edges={{0,2}};
    const auto original=compute_patch_unwrap(mesh,180.f,0.f,layer.lscm_seam_edges);
    require(original.chart_count==2,"Authoring fixture did not create a real UV seam");
    PatchUnwrap edited=original;
    int first=-1,other=-1;
    for (size_t i=0;i<original.source_vertex.size();++i)
        if (original.source_vertex[i]==0) {
            if (first<0) first=int(i);
            else if (original.vertex_chart[i]!=original.vertex_chart[size_t(first)]) other=int(i);
        }
    require(first>=0 && other>=0,"LSCM seam did not duplicate the shared vertex");
    const auto copies=texture_uv_vertex_copies(edited,first);
    require(std::find(copies.begin(),copies.end(),other)==copies.end(),"Vertex selection crossed a UV seam");
    std::vector<TextureIsland> islands(size_t(original.chart_count));
    islands[size_t(original.vertex_chart[size_t(first)])].rotation_deg=90.f;
    islands[size_t(original.vertex_chart[size_t(first)])].scale=2.f;
    const Vec2f before=apply_island_transform(edited.uvs[size_t(first)],edited.vertex_chart[size_t(first)],edited,islands);
    move_texture_uv_vertices(edited,islands,copies,Vec2f(2.f,4.f));
    const Vec2f after=apply_island_transform(edited.uvs[size_t(first)],edited.vertex_chart[size_t(first)],edited,islands);
    require((after-before-Vec2f(2.f,4.f)).norm()<1e-5f && edited.uvs[size_t(other)]==original.uvs[size_t(other)],
            "Vertex drag ignored island rotation/scale or moved the other seam copy");
    set_texture_uv_override(layer,first,edited.uvs[size_t(first)]);
    set_texture_uv_override(layer,first,edited.uvs[size_t(first)]);
    require(layer.lscm_uv_overrides.size()==1 && layer.lscm_uv_overrides[0].first==-(first+1),
            "UV edit did not use the fixed-upstream per-unwrapped-copy key");
    PatchUnwrap applied=original;
    apply_lscm_uv_overrides(applied,layer.lscm_uv_overrides);
    require(applied.uvs[size_t(first)]==edited.uvs[size_t(first)] && applied.uvs[size_t(other)]==original.uvs[size_t(other)],
            "Actual LSCM consumer did not apply the edited copy independently");
    const auto before_corners=compute_lscm_corner_uvs(mesh,TextureDisplacementLayer(layer));
    auto baseline=layer; baseline.lscm_uv_overrides.clear();
    const auto baseline_corners=compute_lscm_corner_uvs(mesh,baseline);
    require(before_corners!=baseline_corners,"Persisted UV edit never reached bake's actual corner sampler");
    PatchUnwrap corners;
    corners.uvs={Vec2f(0,0),Vec2f(0,0),Vec2f(0,0)};
    corners.source_vertex={7,7,7}; corners.vertex_chart={0,0,1};
    require(texture_uv_vertex_copies(corners,0)==std::vector<int>({0,1}),
            "ModelUV corner selection split a connected vertex or crossed its imported seam");
    // Two selected seam copies move together in placed coordinates even though
    // their charts have different transforms. Persist each raw edit and inspect
    // the same corner-UV consumer used by Bake.
    TextureUVSelection selection;
    PatchUnwrap multi=original;
    require(selection.select(multi,first,-1,false,false) && selection.select(multi,other,-1,true,false),
            "Shift did not add a vertex across independent UV charts");
    const auto selected=selection.vertices(multi);
    require(selection.select(multi,first,-1,false,false) && selection.vertices(multi)==selected,
            "Plain press on selected vertex collapsed the drag group");
    const auto first_before=apply_island_transform(multi.uvs[size_t(first)],multi.vertex_chart[size_t(first)],multi,islands);
    const auto other_before=apply_island_transform(multi.uvs[size_t(other)],multi.vertex_chart[size_t(other)],multi,islands);
    const Vec2f delta(.75f,-.5f);
    move_texture_uv_vertices(multi,islands,selected,delta);
    require((apply_island_transform(multi.uvs[size_t(first)],multi.vertex_chart[size_t(first)],multi,islands)-first_before-delta).norm()<1e-5f &&
            (apply_island_transform(multi.uvs[size_t(other)],multi.vertex_chart[size_t(other)],multi,islands)-other_before-delta).norm()<1e-5f,
            "Multivertex drag failed to invert each island's independent transform");
    TextureDisplacementLayer multi_layer=baseline;
    for (int vertex:selected) set_texture_uv_override(multi_layer,vertex,multi.uvs[size_t(vertex)]);
    PatchUnwrap persisted=original;
    apply_lscm_uv_overrides(persisted,multi_layer.lscm_uv_overrides);
    require(persisted.uvs==multi.uvs && compute_lscm_corner_uvs(mesh,multi_layer)!=baseline_corners,
            "Multiple vertex overrides failed to reach the real corner-UV consumer");
    require(!selection.select(multi,other,-1,false,true) && selection.vertices(multi)==texture_uv_vertex_copies(multi,first),
            "Ctrl/Cmd removal removed the wrong selected UV copy");
    require(!selection.select(multi,-1,-1,true,false) && !selection.vertices(multi).empty(),
            "Shift-clicking empty space cleared the vertex group");
    require(!selection.select(multi,-1,-1,false,false) && selection.vertices(multi).empty(),
            "Plain empty click retained the vertex group");

    // Real ModelUV corner aliases: two adjacent edges share a vertex. Toggling
    // either edge must not drop their common endpoint from the remaining edge.
    PatchUnwrap edge_layout;
    edge_layout.uvs={Vec2f(0,0),Vec2f(1,0),Vec2f(1,1),Vec2f(0,0),Vec2f(1,1),Vec2f(0,1),Vec2f(0,0),Vec2f(1,0),Vec2f(1,1)};
    edge_layout.source_vertex={0,1,2,0,2,3,0,1,2};
    edge_layout.vertex_chart={0,0,0,0,0,0,1,1,1};
    edge_layout.chart_centroid={Vec2f(.5f,.5f),Vec2f(.5f,.5f)}; edge_layout.chart_count=2;
    selection.select(edge_layout,0,1,false,false);
    selection.select(edge_layout,1,2,true,false);
    require(selection.vertices(edge_layout)==std::vector<int>({0,1,2,3,4}),
            "Edge multiselection omitted corner aliases, duplicated shared vertices or crossed a seam");
    require(selection.select(edge_layout,2,1,false,false) && selection.vertices(edge_layout).size()==5,
            "Reversed edge hit lost the existing multiselection");
    require(!selection.select(edge_layout,3,1,false,true) && selection.vertices(edge_layout)==std::vector<int>({1,2,4}),
            "Toggling an alias edge incorrectly removed a shared endpoint");
    const auto edge_before=edge_layout.uvs;
    move_texture_uv_vertices(edge_layout,std::vector<TextureIsland>(2),selection.vertices(edge_layout),delta);
    for (size_t vertex=0;vertex<edge_before.size();++vertex) {
        const bool moved=vertex==1 || vertex==2 || vertex==4;
        require((edge_layout.uvs[vertex]-edge_before[vertex]-(moved?delta:Vec2f::Zero())).norm()<1e-6f,
                "Edge drag moved a shared corner twice or moved an unselected seam copy");
    }
    Model model; auto* object=model.add_object(); object->add_instance();
    auto* volume=object->add_volume(TriangleMesh(mesh));
    std::ifstream input(fixtures/"texture-rgb.png",std::ios::binary);
    require(bool(input),"Missing authoring image fixture");
    auto bytes=std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    TextureEditSession session(*volume); session.add_layer("UV","memory://uv",bytes);
    auto& saved=*session.layer(0);
    saved.projection_method=TextureProjectionMethod::LSCM; saved.lscm_seam_angle_deg=180.f;
    saved.lscm_seam_edges=layer.lscm_seam_edges; saved.lscm_uv_overrides=layer.lscm_uv_overrides;
    session.apply(*volume);
    TextureEditSession reopened(*volume);
    require(reopened.layer(0)->lscm_seam_edges==saved.lscm_seam_edges &&
            reopened.layer(0)->lscm_uv_overrides==saved.lscm_uv_overrides,
            "Native Save/reopen lost authored seams or per-copy UV edits");
}
