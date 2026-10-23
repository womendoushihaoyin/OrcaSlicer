#include "libslic3r/Model.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace Slic3r;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void write(const std::filesystem::path& path, const std::string& contents)
{
    std::ofstream file(path);
    file << contents;
    require(bool(file), "Could not write OBJ UV fixture");
}
}

void test_texture_obj(const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);
    const auto obj = output / "model-uv.obj";
    write(obj, "v 0 0 0\nv 2 0 0\nv 2 2 0\nv 0 2 0\n"
               "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
               "f 1/1 2/2 3/3 4/4\n");
    Model model;
    model.set_backup_path((output / "uv-backup").u8string());
    ObjInfo info;
    std::string message;
    require(load_obj(obj.u8string().c_str(), &model, info, message), "Failed to import an OBJ with a standalone UV channel");
    const auto& volume = *model.objects.front()->volumes.front();
    const std::vector<Vec2f> expected{Vec2f(0,1), Vec2f(1,1), Vec2f(1,0), Vec2f(0,1), Vec2f(1,0), Vec2f(0,0)};
    require(volume.has_texture_uv() && volume.texture_uv == expected, "OBJ quad UVs or image V-axis were not preserved without MTL");
    require(info.uvs.size() == 2 && info.uvs[1][2] == Vec2f(0,1), "Second quad triangle did not use its own corners");
    model.add_default_instances();

    // UVs are useful before any texture has been chosen, so an otherwise empty layer stack must
    // still survive the real project archive format.
    const auto archive = (output / "obj-uv.3mf").u8string();
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_settings_id", new ConfigOptionStrings({"PLA"}));
    StoreParams params;
    params.path = archive.c_str(); params.model = &model; params.config = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel;
    require(store_bbs_3mf(params), "Failed to save imported OBJ UVs to 3MF");
    Model restored;
    restored.set_backup_path((output / "uv-restored-backup").u8string());
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    struct ArchiveResources {
        PlateDataPtrs plates;
        std::vector<Preset*> presets;
        ~ArchiveResources() { release_PlateData_list(plates); for (auto* preset : presets) delete preset; }
    } resources;
    bool is_bbs = false;
    Semver version;
    require(load_bbs_3mf(archive.c_str(), &config, &substitutions, &restored, &resources.plates, &resources.presets,
                        &is_bbs, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig), "Failed to reload UV-only 3MF");
    require(restored.objects.size() == 1 && restored.objects[0]->volumes[0]->texture_uv == expected,
            "3MF lost the imported per-corner UV channel when no texture layer existed");

    // Missing vt and out-of-range vt are common in mixed OBJ exports. Both keep one UV triple per
    // emitted face, using the upstream zero-coordinate fallback rather than reading out of bounds.
    write(obj, "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt .2 .3\nf 1/1 2/99 3\n");
    Model partial;
    ObjInfo partial_info;
    require(load_obj(obj.u8string().c_str(), &partial, partial_info, message), "OBJ with partial vt indices failed to import");
    const auto& partial_uv = partial.objects.front()->volumes.front()->texture_uv;
    require(partial_uv.size() == 3 && partial_uv[1] == Vec2f(0,1) && partial_uv[2] == Vec2f(0,1),
            "Invalid OBJ vt index was not bounded or lost face alignment");

    write(obj, "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt .2 .3\nvt .4 .5\nvt .6 .7\nf -3/-3 -2/-2 -1/-1\n");
    Model relative;
    ObjInfo relative_info;
    require(load_obj(obj.u8string().c_str(), &relative, relative_info, message), "OBJ relative UV indices failed to import");
    const auto& relative_uv = relative.objects.front()->volumes.front()->texture_uv;
    require(relative_uv.size() == 3 && (relative_uv[0] - Vec2f(.2f,.7f)).norm() < 1e-6f &&
            (relative_uv[2] - Vec2f(.6f,.3f)).norm() < 1e-6f, "Negative OBJ vt index used the wrong coordinate stride");
}
