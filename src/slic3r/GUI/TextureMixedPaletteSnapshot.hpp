#pragma once
#include "TextureMixRecipes.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <type_traits>

namespace Slic3r::GUI {

// Owned by formal UndoRedo::SnapshotData. Nothing is looked up by a global
// timestamp/filament-ID registry, so trimming or branching history is safe.
struct TextureMixedPaletteSnapshot
{
    MixedFilamentManager manager;
    std::string project_definitions;
    std::string print_definitions;
    bool project_has_definitions = false;
    bool print_has_definitions = false;
    std::vector<std::string> physical_presets;
    std::vector<std::string> physical_colors;
};

inline std::shared_ptr<const TextureMixedPaletteSnapshot> capture_texture_mixed_palette(
    const MixedFilamentManager& manager,const DynamicPrintConfig& project,const DynamicPrintConfig& print,
    std::vector<std::string> physical_presets,std::vector<std::string> physical_colors)
{
    auto result=std::make_shared<TextureMixedPaletteSnapshot>();
    result->manager=manager;
    result->project_has_definitions=project.has("mixed_filament_definitions");
    result->print_has_definitions=print.has("mixed_filament_definitions");
    if(result->project_has_definitions)result->project_definitions=project.opt_string("mixed_filament_definitions");
    if(result->print_has_definitions)result->print_definitions=print.opt_string("mixed_filament_definitions");
    result->physical_presets=std::move(physical_presets);result->physical_colors=std::move(physical_colors);
    return result;
}

inline void restore_texture_palette_definitions(const TextureMixedPaletteSnapshot& snapshot,
    DynamicPrintConfig& project,DynamicPrintConfig& print)
{
    if(snapshot.project_has_definitions)
        project.set_key_value("mixed_filament_definitions",new ConfigOptionString(snapshot.project_definitions));
    else project.erase("mixed_filament_definitions");
    if(snapshot.print_has_definitions)
        print.set_key_value("mixed_filament_definitions",new ConfigOptionString(snapshot.print_definitions));
    else print.erase("mixed_filament_definitions");
}

inline void prepare_texture_palette_restore(const TextureMixedPaletteSnapshot& snapshot,
    MixedFilamentManager& manager,DynamicPrintConfig& project,DynamicPrintConfig& print)
{
    manager=snapshot.manager;
    restore_texture_palette_definitions(snapshot,project,print);
}

// Prepare all mesh/paint transfers and all configuration allocations on owned
// copies. Publishing is a non-allocating swap; leaving scope before finish()
// restores the old volume, manager and both settings strings together.
class TexturePreparedTransaction
{
public:
    TexturePreparedTransaction(Model& model,size_t object_index,size_t volume_index,
        MixedFilamentManager& manager,DynamicPrintConfig& project,DynamicPrintConfig& print,
        const TextureEditSession& session,TextureMixCommit commit)
        : m_model(model),m_object_index(object_index),m_volume_index(volume_index),
          m_manager(manager),m_project(project),m_print(print),m_prepared(model),m_next_manager(std::move(commit.manager)),
          m_next_project(project),m_next_print(print)
    {
        static_assert(std::is_nothrow_move_constructible_v<MixedFilamentManager> &&
                      std::is_nothrow_move_assignable_v<MixedFilamentManager>);
        if(object_index>=model.objects.size() || volume_index>=model.objects[object_index]->volumes.size())
            throw std::runtime_error("The texture part no longer exists");
        m_original_volume=model.objects[object_index]->volumes[volume_index];
        if(!session.source_is_current(*m_original_volume)) throw std::runtime_error("The texture part changed before committing");
        session.apply(*m_prepared.objects[object_index]->volumes[volume_index],std::move(commit.bake.mesh),commit.bake.texture_filaments);
        m_next_project.set_key_value("mixed_filament_definitions",new ConfigOptionString(commit.definitions));
        m_next_print.set_key_value("mixed_filament_definitions",new ConfigOptionString(commit.definitions));
    }
    TexturePreparedTransaction(const TexturePreparedTransaction&)=delete;
    TexturePreparedTransaction& operator=(const TexturePreparedTransaction&)=delete;
    ~TexturePreparedTransaction() {if(m_published && !m_finished)swap_state();}
    void publish()
    {
        if(m_published)throw std::logic_error("Texture transaction was already published");
        if(m_object_index>=m_model.objects.size() || m_volume_index>=m_model.objects[m_object_index]->volumes.size() ||
           m_model.objects[m_object_index]->volumes[m_volume_index]!=m_original_volume)
            throw std::runtime_error("The texture part changed before publishing");
        swap_state();m_published=true;
    }
    void finish() noexcept {m_finished=m_published;}
private:
    void swap_state() noexcept
    {
        std::swap(m_model.objects[m_object_index]->volumes[m_volume_index],m_prepared.objects[m_object_index]->volumes[m_volume_index]);
        m_model.update_links_bottom_up_recursive();m_prepared.update_links_bottom_up_recursive();
        std::swap(m_manager,m_next_manager);m_project.swap(m_next_project);m_print.swap(m_next_print);
        m_model.objects[m_object_index]->invalidate_bounding_box();
    }
    Model& m_model;
    size_t m_object_index,m_volume_index;
    ModelVolume* m_original_volume=nullptr;
    MixedFilamentManager& m_manager;
    DynamicPrintConfig& m_project;
    DynamicPrintConfig& m_print;
    Model m_prepared;
    MixedFilamentManager m_next_manager;
    DynamicPrintConfig m_next_project,m_next_print;
    bool m_published=false,m_finished=false;
};

} // namespace Slic3r::GUI
