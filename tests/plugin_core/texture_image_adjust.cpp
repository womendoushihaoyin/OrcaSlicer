#include "slic3r/GUI/TextureImageAdjustment.hpp"
#include "slic3r/GUI/TextureEditHistory.hpp"
#include "slic3r/GUI/TextureColorBake.hpp"
#include <wx/image.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
std::shared_ptr<std::vector<unsigned char>> image_bytes(const std::filesystem::path& path)
{
    std::ifstream stream(path,std::ios::binary);
    require(bool(stream),"Missing adjustment image fixture");
    return std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>());
}
float maximum_z(const indexed_triangle_set& mesh)
{
    float value=std::numeric_limits<float>::lowest();
    for (const auto& vertex:mesh.vertices) value=std::max(value,vertex.z());
    return value;
}
indexed_triangle_set bake(const TextureEditSession& session)
{
    return build_texture_displacement(session.source->its,session.layers,session.masks,session.options,{},nullptr,
                                      session.volume_to_world,nullptr,nullptr,&session.model_uv);
}
}
void test_texture_image_adjust(const std::filesystem::path& fixtures)
{
    wxInitAllImageHandlers();
    struct ImageHandlerGuard { ~ImageHandlerGuard() { wxImage::CleanUpHandlers(); } } handler_guard;
    TextureDisplacementLayer grey;
    grey.image_data=image_bytes(fixtures/"texture-gray.png"); grey.name="灰度图像调整";
    const auto raw=decode_height_texture(grey);
    require(!raw.empty() && !raw.has_color(),"Adjustment fixture is not grayscale");
    grey.smoothing=1.f; grey.preview_adjust.brightness=.6f;
    TextureImageAdjustment raw_edit(grey);
    require(raw_edit.source().pixels==raw.pixels,"Image adjustments compounded layer smoothing or a previous temporary adjustment");
    raw_edit.settings.brightness=.2f; raw_edit.settings.contrast=.3f;
    raw_edit.settings.blur=.1f; raw_edit.settings.sharpness=.4f; raw_edit.settings.hue_deg=90.f;
    grey.smoothing=0.f;
    raw_edit.preview(grey);
    const auto expected=apply_texture_adjustments(raw,raw_edit.settings);
    require(decode_height_texture(grey).pixels==expected.pixels && !decode_height_texture(grey).has_color(),
            "Live grayscale adjustment did not reach actual decode or promoted grey to RGB");
    raw_edit.preview(grey);
    require(decode_height_texture(grey).pixels==expected.pixels,"Repeated preview compounded the image adjustment");
    raw_edit.cancel(grey);
    require(grey.preview_adjust.brightness==.6f,"Cancel did not restore prior temporary image state");

    Model model; auto* object=model.add_object(); object->add_instance();
    auto* volume=object->add_volume(make_cube(8,8,8));
    TextureEditSession session(*volume);
    session.add_layer("白色图像调整","memory://white",image_bytes(fixtures/"texture-white.png"));
    auto* layer=session.layer(0); layer->projection_method=TextureProjectionMethod::Triplanar;
    layer->depth_mm=.5f;
    session.options.v2_refine_mm=2.f; session.options.v2_max_triangles_k=30;
    session.options.color_despeckle=0;
    TriangleSelector selector(volume->mesh());
    for (size_t face=0;face<volume->mesh().its.indices.size();++face) selector.set_facet(int(face),EnforcerBlockerType::ENFORCER);
    session.masks[0]=selector.serialize();
    const auto unchanged_image=layer->image_data;
    const auto before=bake(session);
    TextureImageAdjustment relief_edit(*layer); relief_edit.settings.brightness=-1.f;
    relief_edit.preview(*layer);
    const auto dark=bake(session);
    require(maximum_z(before)>maximum_z(dark)+.1f,"Brightness changed only a thumbnail, not the actual displacement kernel");
    relief_edit.cancel(*layer);
    require(layer->preview_adjust.identity() && layer->image_data==unchanged_image &&
            std::abs(maximum_z(bake(session))-maximum_z(before))<1e-5f,
            "Cancel failed to restore the image and actual relief");

    TextureEditHistory history(session);
    TextureImageAdjustment committed(*layer); committed.settings.brightness=-.4f;
    committed.preview(*layer);
    const auto preview=decode_height_texture(*layer);
    require(committed.save_copy(*layer),"Save as copy did not produce an adjusted image");
    require(layer->preview_adjust.identity() && layer->image_data!=unchanged_image &&
            decode_height_texture(*layer).pixels==preview.pixels && !decode_height_texture(*layer).has_color(),
            "Committed grayscale image was double-adjusted, changed color type or reused immutable source bytes");
    require(std::filesystem::is_regular_file(std::filesystem::u8path(layer->path)),"Adjusted image was not written to the real user texture library");
    const auto saved_path=layer->path;
    const auto saved_image=layer->image_data;
    require(history.record(session,"Adjust texture image") && history.undo(session) && session.layer(0)->image_data==unchanged_image,
            "Image adjustment was not one reversible editor action");
    require(history.redo(session) && session.layer(0)->image_data==saved_image,"Redo did not restore the adjusted image");
    session.apply(*volume);
    std::string error;
    require(remove_user_texture(saved_path,error),"Could not remove the independent adjusted library copy");
    TextureEditSession reopened(*volume);
    require(reopened.layer(0)->image_data && decode_height_texture(*reopened.layer(0)).pixels==preview.pixels,
            "Reopening depended on the adjusted library file instead of embedded image bytes");

    layer=session.layer(0); layer->image_data=image_bytes(fixtures/"texture-rgb.png"); layer->color_enabled=true;
    const auto rgb=decode_height_texture(*layer);
    TextureImageAdjustment rgb_edit(*layer);
    rgb_edit.settings.hue_deg=90.f; rgb_edit.settings.brightness=.1f; rgb_edit.settings.contrast=.2f;
    rgb_edit.preview(*layer);
    const auto adjusted_rgb=apply_texture_adjustments(rgb,rgb_edit.settings);
    require(decode_height_texture(*layer).rgb==adjusted_rgb.rgb && decode_height_texture(*layer).pixels==adjusted_rgb.pixels &&
            adjusted_rgb.rgb!=rgb.rgb,"Hue/tonal edits did not drive RGB and luminance together");
    require(rgb_edit.save_copy(*layer),"RGB Save as copy failed");
    require(decode_height_texture(*layer).has_color() && decode_height_texture(*layer).rgb==adjusted_rgb.rgb &&
            decode_height_texture(*layer).pixels==adjusted_rgb.pixels,"RGB commit changed row order, color bytes or luminance");
    const auto rgb_path=layer->path;
    TextureImageAdjustment color_edit(*layer);
    const TexturePalette palette{{1,Vec3f(0,0,0),false},{2,Vec3f(1,1,1),false}};
    color_edit.settings.brightness=1.f; color_edit.preview(*layer);
    const auto white=bake_texture_session(session,palette,{});
    color_edit.settings.brightness=-1.f; color_edit.preview(*layer);
    const auto black=bake_texture_session(session,palette,{});
    require(!white.texture_filaments.empty() && !black.texture_filaments.empty() &&
            std::all_of(white.texture_filaments.begin(),white.texture_filaments.end(),[](uint8_t id) { return id==2; }) &&
            std::all_of(black.texture_filaments.begin(),black.texture_filaments.end(),[](uint8_t id) { return id==1; }),
            "Image adjustments did not reach actual RGB-to-filament bake assignments");
    color_edit.cancel(*layer);
    require(remove_user_texture(rgb_path,error),"Could not remove the independent RGB library copy");
}
