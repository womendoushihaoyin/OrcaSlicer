#include "slic3r/GUI/TextureMixRecipes.hpp"
#include "slic3r/GUI/TextureMixedPaletteSnapshot.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include <wx/image.h>
#include <wx/mstream.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
Vec3f rgb(const std::string& value)
{
    ColorRGB c;require(decode_color(value,c),"Invalid fixture color");return Vec3f(c.r(),c.g(),c.b());
}
TexturePalette palette(const MixedFilamentManager& manager,const MixedFilamentDisplayContext& context)
{
    TexturePalette out;
    for (size_t i=0;i<context.physical_colors.size();++i) out.push_back({int(i+1),rgb(context.physical_colors[i]),false});
    for (const auto& color:manager.display_colors()) out.push_back({int(out.size()+1),rgb(color),true});
    return out;
}
std::shared_ptr<std::vector<unsigned char>> gradient(const MixedFilamentDisplayContext& context)
{
    wxImage image(48,32);
    for(int x=0;x<48;++x) {
        MixedFilament row;row.component_a=1;row.component_b=2;row.mix_b_percent=10+x*80/47;
        const auto color=rgb(compute_mixed_filament_display_color(row,context));
        for(int y=0;y<32;++y) {
            const float value=.1f+.9f*y/31.f;
            image.SetRGB(x,y,uint8_t(color.x()*value*255),uint8_t(color.y()*value*255),uint8_t(color.z()*value*255));
        }
    }
    wxMemoryOutputStream output;require(image.SaveFile(output,wxBITMAP_TYPE_PNG),"Could not encode mix fixture");
    auto bytes=std::make_shared<std::vector<unsigned char>>(output.GetSize());output.CopyTo(bytes->data(),bytes->size());return bytes;
}
indexed_triangle_set plane()
{
    indexed_triangle_set mesh;
    mesh.vertices={Vec3f(0,0,5),Vec3f(12,0,5),Vec3f(12,8,5),Vec3f(0,8,5)};
    mesh.indices={Vec3i32(0,1,2),Vec3i32(0,2,3)};return mesh;
}
}

void test_texture_mix_recipes(const std::filesystem::path& fixtures, const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);
    wxInitAllImageHandlers();
    struct Handlers {~Handlers(){wxImage::CleanUpHandlers();}} handlers;
    MixedFilamentDisplayContext context;
    context.num_physical=2;context.physical_colors={"#FF0000","#0000FF"};context.nozzle_diameters={.4,.4};
    MixedFilamentManager source;source.set_display_context(context);
    source.apply_gradient_settings(0,.04f,.16f,false);
    const auto source_rows=source.mixed_filaments();
    Model model;auto* object=model.add_object();object->add_instance();
    auto* volume=object->add_volume(TriangleMesh(plane()));
    auto* sibling=object->add_volume(make_cube(3,3,3));
    auto* other_object=model.add_object();other_object->add_instance();
    auto* other_volume=other_object->add_volume(make_cube(2,2,2));
    const auto sibling_id=sibling->id(),other_id=other_volume->id(),original_id=volume->id();
    volume->texture_uv={Vec2f(0,0),Vec2f(1,0),Vec2f(1,1),Vec2f(0,0),Vec2f(1,1),Vec2f(0,1)};
    TextureEditSession session(*volume);session.add_layer("Mixed gradient","memory://mixed-gradient",gradient(context));
    auto* layer=session.layer(0);layer->color_enabled=true;layer->projection_method=TextureProjectionMethod::ModelUV;
    {std::ofstream image_file(output/"auto-mix-gradient.png",std::ios::binary);
     image_file.write(reinterpret_cast<const char*>(layer->image_data->data()),std::streamsize(layer->image_data->size()));}
    layer->depth_mm=.1f;layer->tiling_scale=1.f;
    session.options.v2_refine_mm=1.f;session.options.v2_max_triangles_k=20;session.options.color_despeckle=0;
    session.options.color_mix_enabled=true;session.options.color_mix_count=4;
    TriangleSelector selector(volume->mesh());selector.set_facet(0,EnforcerBlockerType::ENFORCER);selector.set_facet(1,EnforcerBlockerType::ENFORCER);
    session.masks[0]=selector.serialize();
    require(!analyze_texture_detail(*layer).flat_colors,"Mix gradient fixture must exercise nonflat RGB ranking");
    auto plan=plan_texture_mix_palette(session.layers,palette(source,context),source,context,{},4);
    require(plan.recipes.size()>1,"Gradient did not produce useful mixed recipes");
    require(source.mixed_filaments()==source_rows && source.enabled_count()==0,"Planning mutated the live source manager");
    for (const auto& recipe:plan.recipes) {
        require(plan.preview_manager.is_mixed(unsigned(recipe.preview_filament_id),2),"Preview recipe has no real owned slot");
        std::set<unsigned> physical;
        for(int l=0;l<12;++l)physical.insert(plan.preview_manager.resolve(unsigned(recipe.preview_filament_id),2,l));
        require(physical==std::set<unsigned>({1,2}),"A planned recipe did not resolve to both actual physical tools");
    }
    auto local_context=context;
    local_context.nozzle_diameters={.4,.6};local_context.preview_settings.local_z_mode=true;
    local_context.preview_settings.preferred_a_height=.06f;local_context.preview_settings.preferred_b_height=.14f;
    local_context.component_bias_enabled=true;
    const auto local_plan=plan_texture_mix_palette(session.layers,palette(source,local_context),source,local_context,{1,true},2);
    require(!local_plan.recipes.empty(),"Local-Z display policy failed to rank real mixed recipes");
    for(const auto& recipe:local_plan.recipes) {
        const auto* row=local_plan.preview_manager.mixed_filament_from_id(unsigned(recipe.preview_filament_id),2);
        const auto entry=std::find_if(local_plan.palette.begin(),local_plan.palette.end(),[&](const auto& c){return c.filament_id==recipe.preview_filament_id;});
        require(row && entry!=local_plan.palette.end() &&
            entry->rgb.isApprox(rgb(compute_mixed_filament_display_color(*row,local_context)),1e-6f),
            "Local-Z candidate color diverged from the actual manager's nozzle/cadence display policy");
    }
    const auto canceled=plan_texture_mix_palette(session.layers,palette(source,context),source,context,{},4,[](int){return false;});
    require(canceled.canceled && source.enabled_count()==0,"Canceled planning created live recipes");

    // Only a late candidate is consumed: all earlier preview slots must disappear
    // and its facet ID must remap to the first real appended slot.
    TextureColorBakeResult one;one.mesh=plane();
    const int last=plan.recipes.back().preview_filament_id;
    one.texture_filaments={uint8_t(last),1};one.preview_filaments=one.texture_filaments;
    const auto sparse=resolve_texture_mix_commit(plan,std::move(one));
    require(sparse.created_stable_ids.size()==1 && sparse.manager.enabled_count()==1 &&
            sparse.bake.texture_filaments==std::vector<uint8_t>({3,1}),"Unused preview recipes consumed slots or facet IDs were not remapped");
    const auto* sparse_row=sparse.manager.mixed_filament_from_id(3,2);
    require(sparse_row && sparse_row->stable_id==sparse.created_stable_ids[0] && sparse_row->stable_id==1,"Preview consumed the live stable-ID allocator or new recipe lost identity");

    auto actual=bake_texture_session(session,plan.palette,{});
    require(actual.mesh.indices.size()>2,"Mixed color fixture did not exercise the actual refined bake");
    require(std::any_of(actual.texture_filaments.begin(),actual.texture_filaments.end(),[](auto id){return id>2;}),
            "Actual texture bake never sampled an automatically planned mixed color");
    auto commit=resolve_texture_mix_commit(plan,std::move(actual));
    require(!commit.created_stable_ids.empty() && commit.created_stable_ids.size()<=plan.recipes.size(),"Actual bake did not allocate used recipe slots");
    const auto expected=commit.bake.texture_filaments;
    MixedFilamentManager live_manager=source;
    DynamicPrintConfig project_config,print_config;
    project_config.set_key_value("mixed_filament_definitions",new ConfigOptionString(""));
    print_config.set_key_value("mixed_filament_definitions",new ConfigOptionString(""));
    const auto before_palette=capture_texture_mixed_palette(live_manager,project_config,print_config,{"red","blue"},context.physical_colors);
    UndoRedo::SnapshotData before_formal;
    before_formal.texture_mixed_palette=before_palette;
    const auto before_mesh=volume->mesh_ptr();
    bool rolled_back=false;
    try {
        TexturePreparedTransaction pending(model,0,0,live_manager,project_config,print_config,session,commit);
        require(model.objects[0]->volumes[0]==volume && live_manager.enabled_count()==0,"Preparing a bake modified the project");
        pending.publish();
        require(live_manager.enabled_count()>0 && model.objects[0]->volumes[0]!=volume,"Publishing did not commit real geometry and slots");
        throw std::runtime_error("injected after publication");
    } catch(const std::runtime_error&) {rolled_back=true;}
    require(rolled_back && model.objects[0]->volumes[0]==volume && volume->mesh_ptr()==before_mesh &&
            live_manager.enabled_count()==0 && object->volumes[1]==sibling && model.objects[1]==other_object &&
            other_object->volumes[0]==other_volume && sibling->id()==sibling_id && other_volume->id()==other_id &&
            project_config.opt_string("mixed_filament_definitions").empty() &&
            print_config.opt_string("mixed_filament_definitions").empty(),"Failed bake did not roll back mesh, manager and both configurations");
    bool invalid_rejected=false;
    try {
        auto invalid=commit;invalid.bake.texture_filaments.push_back(1);
        TexturePreparedTransaction invalid_transaction(model,0,0,live_manager,project_config,print_config,session,std::move(invalid));
    } catch(const std::exception&) {invalid_rejected=true;}
    require(invalid_rejected && live_manager.enabled_count()==0 && volume->mesh_ptr()==before_mesh,"Preflight failure leaked a recipe or geometry");
    {
        TexturePreparedTransaction transaction(model,0,0,live_manager,project_config,print_config,session,commit);
        transaction.publish();transaction.finish();
    }
    volume=model.objects[0]->volumes[0];
    require(volume->get_object()==model.objects[0] && volume->id()!=original_id && object->volumes[1]==sibling &&
            model.objects[1]==other_object && other_object->volumes[0]==other_volume && sibling->get_object()==object &&
            other_volume->get_object()==other_object && sibling->id()==sibling_id && other_volume->id()==other_id &&
            live_manager.mixed_filaments()==commit.manager.mixed_filaments(),
            "Committed volume parent or manager identities are invalid");
    const auto after_palette=capture_texture_mixed_palette(live_manager,project_config,print_config,{"red","blue"},context.physical_colors);
    UndoRedo::SnapshotData after_formal;
    after_formal.texture_mixed_palette=after_palette;
    prepare_texture_palette_restore(*before_formal.texture_mixed_palette,live_manager,project_config,print_config);
    require(live_manager.enabled_count()==0 && project_config.opt_string("mixed_filament_definitions").empty(),"Before memento did not restore old palette");
    prepare_texture_palette_restore(*after_formal.texture_mixed_palette,live_manager,project_config,print_config);
    require(live_manager.mixed_filaments()==commit.manager.mixed_filaments() &&
            project_config.opt_string("mixed_filament_definitions")==commit.definitions &&
            print_config.opt_string("mixed_filament_definitions")==commit.definitions,"After memento lost stable IDs or settings");
    DynamicPrintConfig absent_project,different_print;
    different_print.set_key_value("mixed_filament_definitions",new ConfigOptionString("legacy-preserved-text"));
    const auto exact_fields=capture_texture_mixed_palette(live_manager,absent_project,different_print,{"red","blue"},context.physical_colors);
    absent_project.set_key_value("mixed_filament_definitions",new ConfigOptionString(commit.definitions));
    different_print.set_key_value("mixed_filament_definitions",new ConfigOptionString(commit.definitions));
    restore_texture_palette_definitions(*exact_fields,absent_project,different_print);
    require(!absent_project.has("mixed_filament_definitions") && different_print.opt_string("mixed_filament_definitions")=="legacy-preserved-text",
        "Palette refresh did not preserve the exact presence and separate value of both definition fields");
    TriangleSelector painted(volume->mesh());painted.deserialize(volume->mmu_segmentation_facets.get_data());
    for(size_t f=0;f<expected.size();++f) if(expected[f]>2)
        require(commit.manager.is_mixed(expected[f],2),"Committed MMU facet references an unallocated recipe");

    // A real project archive owns the palette definitions, MMU states and the
    // editable recipe/image together. Reload must not depend on the source PNG.
    model.set_backup_path((output / "source-backup").u8string());
    auto archive_config=DynamicPrintConfig::full_print_config();
    archive_config.set_num_filaments(2);
    archive_config.set_key_value("filament_settings_id",new ConfigOptionStrings({"red","blue"}));
    archive_config.set_key_value("filament_colour",new ConfigOptionStrings(context.physical_colors));
    archive_config.set_key_value("mixed_filament_definitions",new ConfigOptionString(commit.definitions));
    const auto archive_path=(output / std::filesystem::u8path("混色事务.3mf")).u8string();
    StoreParams store;store.path=archive_path.c_str();store.model=&model;store.config=&archive_config;
    store.strategy=SaveStrategy::Zip64|SaveStrategy::Silence|SaveStrategy::SplitModel;
    require(store_bbs_3mf(store),"Mixed project archive could not be saved");
    struct ArchiveResources {
        PlateDataPtrs plates;std::vector<Preset*> presets;
        ~ArchiveResources(){release_PlateData_list(plates);for(auto* preset:presets)delete preset;}
    } resources;
    Model reopened;reopened.set_backup_path((output/"reopened-backup").u8string());
    DynamicPrintConfig reopened_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    bool is_bbs=false;Semver version;
    require(load_bbs_3mf(archive_path.c_str(),&reopened_config,&substitutions,&reopened,&resources.plates,
        &resources.presets,&is_bbs,&version,nullptr,LoadStrategy::LoadModel|LoadStrategy::LoadConfig),
        "Mixed project archive could not be reopened");
    require(reopened.objects.size()==model.objects.size() && reopened.objects[0]->volumes.size()==object->volumes.size(),
        "Mixed project archive lost unselected geometry");
    const auto* reopened_volume=reopened.objects[0]->volumes[0];
    const auto reopened_layers=TextureEditSession(*reopened_volume);
    require(reopened_volume->mmu_segmentation_facets.get_data()==volume->mmu_segmentation_facets.get_data() &&
        reopened_layers.options.color_mix_enabled && reopened_layers.options.color_mix_count==4 &&
        reopened_layers.layers.size()==1 && reopened_layers.layer(0)->image_data &&
        *reopened_layers.layer(0)->image_data==*session.layer(0)->image_data &&
        reopened_config.opt_string("mixed_filament_definitions")==commit.definitions,
        "3MF reload lost mixed IDs, embedded image, options or definitions");

    MixedFilamentManager restored;restored.set_display_context(context);
    restored.load_custom_entries(reopened_config.opt_string("mixed_filament_definitions"),context.physical_colors);
    restored.apply_gradient_settings(0,.04f,.16f,false);
    require(restored.mixed_filaments()==commit.manager.mixed_filaments(),"Serialized recipes changed IDs or printing cadence on reload");
    const auto repeat=plan_texture_mix_palette(session.layers,palette(restored,context),restored,context,{},4);
    require(std::any_of(repeat.recipes.begin(),repeat.recipes.end(),[](const auto& recipe){return recipe.existing_filament_id>0;}),
            "Existing equivalent recipes were not reused");
    const auto before_repeat=restored.mixed_filaments();
    TextureColorBakeResult again;again.mesh=plane();
    auto reused=std::find_if(repeat.recipes.begin(),repeat.recipes.end(),[](const auto& recipe){return recipe.existing_filament_id>0;});
    again.texture_filaments.assign(2,uint8_t(reused->preview_filament_id));again.preview_filaments=again.texture_filaments;
    const auto reuse_commit=resolve_texture_mix_commit(repeat,std::move(again));
    require(reuse_commit.created_stable_ids.empty() && reuse_commit.manager.mixed_filaments()==before_repeat,
            "Reusing an existing recipe duplicated or renumbered a filament slot");

    auto capped=restored;
    MixedFilamentBatchEntry filler;filler.component_a=1;filler.component_b=2;filler.mix_b_percent=1;
    capped.add_batch_custom_filaments(std::vector<MixedFilamentBatchEntry>(MAXIMUM_FILAMENT_NUMBER-capped.total_filaments(2),filler),context.physical_colors);
    capped.apply_gradient_settings(0,.04f,.16f,false);
    const auto capped_rows=capped.mixed_filaments();
    require(capped.total_filaments(2)==MAXIMUM_FILAMENT_NUMBER,"Full-palette fixture did not reach its slot limit");
    const auto capped_plan=plan_texture_mix_palette(session.layers,palette(capped,context),capped,context,{},4);
    require(!capped_plan.recipes.empty() && capped_plan.preview_manager.total_filaments(2)==MAXIMUM_FILAMENT_NUMBER &&
        std::all_of(capped_plan.recipes.begin(),capped_plan.recipes.end(),[](const auto& row){return row.existing_filament_id>0;}) &&
        capped.mixed_filaments()==capped_rows,"Full palette allocated, renumbered or failed to reuse an existing compatible recipe");

    wxImage flat_image(8,8);flat_image.SetRGB(wxRect(0,0,8,8),180,20,120);
    wxMemoryOutputStream flat_output;require(flat_image.SaveFile(flat_output,wxBITMAP_TYPE_PNG),"Cannot encode flat fixture");
    TextureDisplacementLayer flat=*layer;
    flat.image_data=std::make_shared<std::vector<unsigned char>>(flat_output.GetSize());
    flat_output.CopyTo(flat.image_data->data(),flat.image_data->size());
    require(analyze_texture_detail(flat).flat_colors,"Flat fixture changed");
    require(plan_texture_mix_palette({flat},palette(source,context),source,context,{},4).recipes.empty(),"Flat RGB colors created unnecessary mixes");
    require(source.enabled_count()==0,"Preview, bake resolution, or cancellation mutated the source palette");
    auto full_context=context;
    full_context.num_physical=MAXIMUM_FILAMENT_NUMBER;
    full_context.physical_colors.assign(MAXIMUM_FILAMENT_NUMBER,"#FF0000");
    full_context.nozzle_diameters.assign(MAXIMUM_FILAMENT_NUMBER,.4);
    require(plan_texture_mix_palette(session.layers,palette(source,full_context),source,full_context,{},4).recipes.empty(),
            "A palette with every physical slot occupied attempted to allocate candidates");
    bool bad=false;
    try {
        TextureColorBakeResult invalid;invalid.mesh=plane();invalid.texture_filaments={0,1};
        invalid.preview_filaments={uint8_t(plan.recipes.front().preview_filament_id),1};
        resolve_texture_mix_commit(plan,std::move(invalid));
    } catch(const std::runtime_error&) {bad=true;}
    require(bad,"An orphaned previous MMU state silently became a new preview recipe");

    // Exercise the actual slicing/export consumer with a closed solid whose
    // faces sample a real automatically planned mixture. Pure IDs are absent
    // on the painted solid, so both physical tools must come from its recipe.
    Model printable;auto* solid=printable.add_object();solid->add_instance();
    auto* solid_volume=solid->add_volume(make_cube(8.,8.,3.));
    solid_volume->texture_uv.assign(solid_volume->mesh().its.indices.size()*3,Vec2f(.5f,.65f));
    TextureEditSession color_only(*solid_volume);
    color_only.add_layer("Auto mixed solid","memory://mixed-gradient",session.layer(0)->image_data);
    *color_only.layer(0)=*session.layer(0);color_only.layer(0)->depth_mm=0.f;
    color_only.options=session.options;color_only.options.pipeline_v2=false;
    TriangleSelector whole(solid_volume->mesh());
    for(size_t f=0;f<solid_volume->mesh().its.indices.size();++f)whole.set_facet(int(f),EnforcerBlockerType::ENFORCER);
    color_only.masks[0]=whole.serialize();
    auto solid_bake=bake_texture_session(color_only,plan.palette,{});
    require(!solid_bake.texture_filaments.empty() && std::all_of(solid_bake.texture_filaments.begin(),solid_bake.texture_filaments.end(),[](auto id){return id>2;}),
        "Closed texture fixture did not sample exclusively generated mixed recipes");
    auto solid_commit=resolve_texture_mix_commit(plan,std::move(solid_bake));
    color_only.apply(*solid_volume,std::move(solid_commit.bake.mesh),solid_commit.bake.texture_filaments);
    auto print_config_full=DynamicPrintConfig::full_print_config();
    print_config_full.set_num_extruders(2);print_config_full.set_num_filaments(2);
    print_config_full.set_key_value("filament_diameter",new ConfigOptionFloats({1.75,1.75}));
    print_config_full.set_key_value("filament_flow_ratio",new ConfigOptionFloats({1.,1.}));
    print_config_full.set_key_value("change_filament_gcode",new ConfigOptionString("T{next_extruder}"));
    print_config_full.set_key_value("filament_colour",new ConfigOptionStrings(context.physical_colors));
    print_config_full.set_key_value("filament_settings_id",new ConfigOptionStrings({"red","blue"}));
    print_config_full.set_key_value("mixed_filament_definitions",new ConfigOptionString(solid_commit.definitions));
    print_config_full.set_key_value("enable_prime_tower",new ConfigOptionBool(false));
    print_config_full.set_key_value("flush_into_infill",new ConfigOptionBool(false));
    print_config_full.set_key_value("flush_into_support",new ConfigOptionBool(false));
    print_config_full.set_key_value("mixed_filament_gradient_mode",new ConfigOptionBool(false));
    print_config_full.set_key_value("mixed_filament_advanced_dithering",new ConfigOptionBool(false));
    print_config_full.set_key_value("layer_height",new ConfigOptionFloat(.2));
    // The existing MMU segmentation consumer requires an explicit width; the
    // bare auto-width default bypasses its usual Flow width fallback.
    print_config_full.set_key_value("outer_wall_line_width",new ConfigOptionFloatOrPercent(.45,false));
    Print print;print.is_BBL_printer()=false;print.set_status_silent();print.set_cancel_callback([]{});
    print.apply(printable,print_config_full);
    require(print.config().filament_diameter.size()==2,"Mixed export fixture did not create two physical filament slots");
    print.restart();print.process();
    GCodeProcessorResult exported;
    const auto gcode=output/"generated-mixed-texture.gcode";
    print.export_gcode(gcode.u8string(),&exported);
    require(print.is_step_done(psGCodeExport) && std::filesystem::file_size(gcode)>1000,
        "Mixed texture did not produce completed real G-code");
    std::set<unsigned> tools;size_t extrusion_moves=0;
    for(const auto& move:exported.moves) {
        require(move.position.allFinite() && std::isfinite(move.delta_extruder),"Mixed G-code contains nonfinite coordinates or extrusion");
        if(move.type==EMoveType::Extrude && move.delta_extruder>0) {
            require(move.extruder_id<2 && move.position.z()>=-1e-5f,"Mixed G-code emitted a virtual tool or invalid Z");
            tools.insert(move.extruder_id);++extrusion_moves;
        }
    }
    require(extrusion_moves>10 && tools==std::set<unsigned>({0,1}),
        "Generated mixed MMU states did not resolve to both physical tools in real G-code");

}
