#include "slic3r/GUI/TexturePaintVisibility.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <iostream>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
indexed_triangle_set walls(float gap = 1.f)
{
    indexed_triangle_set result;
    result.vertices = {Vec3f(0,0,5),Vec3f(2,0,5),Vec3f(0,2,5),
                       Vec3f(0,0,5-gap),Vec3f(2,0,5-gap),Vec3f(0,2,5-gap),
                       Vec3f(4,0,5),Vec3f(6,0,5),Vec3f(4,2,5)};
    result.indices = {Vec3i32(0,1,2),Vec3i32(3,4,5),Vec3i32(6,8,7)};
    return result;
}
}
void test_texture_visibility(const std::filesystem::path& fixtures)
{
    for (float gap : {1.f, .00001f}) {
        const TriangleMesh mesh(walls(gap));
        const AABBMesh rays(mesh);
        require(texture_visible_faces(mesh,rays,Transform3d::Identity(),-Vec3d::UnitZ()) == std::vector<int>{0},
                "Visibility painted an occluded front-facing wall or a back-facing face");
        require(texture_visible_faces(mesh,rays,Transform3d::Identity(),Vec3d::UnitZ()) == std::vector<int>{2},
                "Camera reversal did not replace the visible side");
        require(texture_visible_faces(mesh,rays,Transform3d::Identity(),Vec3d::UnitX()).empty(),
                "Edge-on faces were accepted as front-facing");
        Transform3d frame = Eigen::Translation3d(20.,-5.,17.) * Eigen::AngleAxisd(.7,Vec3d(1,2,3).normalized()) *
                            Eigen::Scaling(-2.,.25,3.);
        const Vec3d world_forward = (frame.linear() * -Vec3d::UnitZ()).normalized();
        require(texture_visible_faces(mesh,rays,frame,world_forward) == std::vector<int>{0},
                "Mirrored non-uniform instance changed facing or occlusion");
    }
    const TriangleMesh coincident(walls(0.f));
    const AABBMesh coincident_rays(coincident);
    require(texture_visible_faces(coincident,coincident_rays,Transform3d::Identity(),-Vec3d::UnitZ()) == std::vector<int>({0,1}),
            "Coplanar hit ties lost an independently visible source face");
    auto thin = walls();
    for (int vertex=0;vertex<3;++vertex) thin.vertices[size_t(vertex)].z()=.001f;
    for (int vertex=3;vertex<6;++vertex) thin.vertices[size_t(vertex)].z()=.001f-1e-9f;
    const TriangleMesh thin_mesh(thin);
    const AABBMesh thin_rays(thin_mesh);
    require(texture_visible_faces(thin_mesh,thin_rays,Transform3d::Identity(),-Vec3d::UnitZ()) == std::vector<int>{0},
            "Float ray distance precision swallowed a distinct thin front wall");
    indexed_triangle_set oblique;
    oblique.vertices = {Vec3f(0,0,0),Vec3f(0,0,1),Vec3f(1,-1,0)};
    oblique.indices = {Vec3i32(0,1,2)};
    const TriangleMesh slope(oblique);
    const AABBMesh slope_rays(slope);
    Transform3d stretch = Transform3d(Eigen::Scaling(10.,.1,1.));
    require(texture_visible_faces(slope,slope_rays,stretch,Vec3d(-1,1,0)).empty(),
            "Facing used the position matrix instead of inverse-transpose normals");
    require(texture_visible_faces(slope,slope_rays,stretch,Vec3d(1,-1,0)) == std::vector<int>{0},
            "Inverse-transpose facing rejected the visible oblique face");
    bool rejected = false;
    stretch.linear()(1,1) = 0.;
    try { texture_visible_faces(slope,slope_rays,stretch,Vec3d::UnitZ()); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected,"Singular camera frame was not rejected");

    const TriangleMesh mesh(walls());
    const AABBMesh rays(mesh);
    TriangleSelector old(mesh);
    for (int face = 0; face < 3; ++face) old.set_facet(face,EnforcerBlockerType::ENFORCER);
    auto mask = old.serialize(); const auto old_mask = mask;
    rejected = false;
    try { mask = texture_visible_mask(mesh,{}); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected && mask == old_mask,"Empty view erased existing paint");
    const auto visible = texture_visible_faces(mesh,rays,Transform3d::Identity(),-Vec3d::UnitZ());
    mask = texture_visible_mask(mesh,visible);
    require(mask != old_mask,"Visible selection did not replace the old whole-object mask");
    TriangleSelector check(mesh); check.deserialize(mask);
    require(check.get_facets(EnforcerBlockerType::ENFORCER).indices.size() == 1,
            "Visible mask was not usable by the real TriangleSelector");

    std::ifstream input(fixtures / "texture-white.png", std::ios::binary);
    require(bool(input),"Missing real texture fixture");
    TextureDisplacementLayer layer;
    layer.slot=0; layer.projection_method=TextureProjectionMethod::ViewProjected; layer.depth_mm=.4f;
    layer.image_data=std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    TextureDisplacementFacetsData previous_masks; previous_masks[0]=old_mask;
    const auto previous_sampler=make_combined_displacement_sampler(mesh.its,{layer},previous_masks);
    require(previous_sampler && std::abs(previous_sampler(Vec3f(.5f,.5f,4),Vec3f::UnitZ())-.4f)<1e-4f,
            "Before-case fixture did not expose projection onto an occluded wall");
    TextureDisplacementFacetsData masks; masks[0]=mask;
    const auto sampler=make_combined_displacement_sampler(mesh.its,{layer},masks);
    require(sampler && std::abs(sampler(Vec3f(.5f,.5f,5),Vec3f::UnitZ())-.4f)<1e-4f &&
            std::abs(sampler(Vec3f(.5f,.5f,4),Vec3f::UnitZ()))<1e-4f,
            "Visible-face paint did not constrain the actual texture displacement sampler");
    TextureDisplacementOptions options; options.pipeline_v2=false; options.displace_border=true;
    const auto baked=build_texture_displacement(mesh.its,{layer},masks,options);
    require(baked.vertices.size()==mesh.its.vertices.size(),"Simple visible-face bake changed fixture topology unexpectedly");
    for (size_t vertex=0;vertex<baked.vertices.size();++vertex) {
        const float expected=mesh.its.vertices[vertex].z()+(vertex<3?.4f:0.f);
        require(std::abs(baked.vertices[vertex].z()-expected)<1e-4f,
                "Actual texture bake displaced hidden or back-facing geometry");
    }
    // The frame's homogeneous map uses the actual clip product. Retina changes
    // logical pixels to physical pixels on both sides, so it cannot move the UVs.
    Eigen::Matrix4d perspective=Eigen::Matrix4d::Zero();
    perspective(0,0)=2.; perspective(1,1)=2.; perspective(2,2)=-1.; perspective(3,2)=-1.;
    const Vec2d viewport(800,600); const Eigen::Vector4d gate(200,150,400,300);
    const auto projective=texture_frame_projection(perspective,viewport,gate);
    require(projective==texture_frame_projection(perspective,viewport*2.,gate*2.),
            "Retina conversion changed the projection frame placement");
    Vec2f uv;
    require(project_uv_projective(projective,Vec3f(0,0,-2),uv) && (uv-Vec2f(.5f,.5f)).norm()<1e-6f &&
            texture_frame_contains(projective,Vec3f(.4f,.4f,-2)) &&
            !texture_frame_contains(projective,Vec3f(.6f,0,-2)) &&
            !texture_frame_contains(projective,Vec3f(0,0,2)),
            "Frame crop or behind-projector rejection disagrees with the actual projective sampler");
    const auto shifted=texture_frame_projection(perspective,viewport,Eigen::Vector4d(400,150,400,300));
    require(project_uv_projective(shifted,Vec3f(0,0,-2),uv) && (uv-Vec2f(0,.5f)).norm()<1e-6f,
            "Moving the projection frame did not move the real UV map");
    rejected=false;
    try { texture_frame_projection(perspective,viewport,Eigen::Vector4d(200,150,0,300)); }
    catch (const std::invalid_argument&) { rejected=true; }
    require(rejected,"An empty frame was accepted");

    // The editor displays the kernel's bake frame. A non-uniform volume must be
    // transformed exactly once; an imported local-space matrix would double it.
    const Transform3d scaled(Eigen::Scaling(2.,3.,4.));
    Eigen::Matrix4d clip=Eigen::Matrix4d::Identity(); clip(0,0)=1./8.; clip(1,1)=1./12.;
    layer.view_project_projective=true; layer.tile_enabled=false;
    layer.view_project_matrix=texture_frame_projection(clip,viewport,Eigen::Vector4d(0,0,800,600));
    require(texture_visible_faces(mesh,rays,scaled,-Vec3d::UnitZ(),&layer.view_project_matrix)==std::vector<int>{0},
            "Frame clip disagreed with transformed visible geometry");
    const auto framed_bake=build_texture_displacement(mesh.its,{layer},masks,options,{},nullptr,scaled);
    for (size_t vertex=0;vertex<framed_bake.vertices.size();++vertex) {
        const float expected=mesh.its.vertices[vertex].z()+(vertex<3?.1f:0.f);
        require(std::abs(framed_bake.vertices[vertex].z()-expected)<1e-4f,
                "Projected-frame bake applied the volume transform twice or displaced a hidden wall");
    }
    const auto outside=texture_frame_projection(clip,viewport,Eigen::Vector4d(900,0,800,600));
    require(texture_visible_faces(mesh,rays,scaled,-Vec3d::UnitZ(),&outside).empty(),
            "Moving the gate outside the object still painted visible faces");
    const auto encoded=texture_displacement_layers_to_json({layer},options);
    std::vector<TextureDisplacementLayer> restored; TextureDisplacementOptions restored_options;
    require(texture_displacement_layers_from_json(encoded,restored,restored_options),"Projection recipe decode failed");
    require(restored.size()==1 && restored[0].view_project_projective &&
            restored[0].view_project_matrix==layer.view_project_matrix && !restored[0].tile_enabled,
            "Projection frame matrix failed the real recipe codec");
    std::cout << "visible_faces=1 hidden_front_faces=1 rejected_back_faces=1 hidden_before=.4 hidden_after=0 actual_bake=PASS\n";
}
