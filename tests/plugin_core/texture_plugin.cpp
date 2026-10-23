#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/host/PluginResourceTextures.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include "libslic3r/TextureDisplacementConsumer.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <wx/image.h>

namespace test_fs = std::filesystem;
using namespace Slic3r;
namespace {
void require(bool ok, const std::string& message)
{
    if (!ok)
        throw std::runtime_error(message);
}
std::vector<unsigned char> read_bytes(const test_fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
PluginDescriptor install_and_load(const test_fs::path& path)
{
    auto& manager = PluginManager::instance();
    PluginDescriptor descriptor;
    std::string error;
    require(manager.install_plugin(path.u8string(), descriptor, error), error);
    manager.rescan_plugins();
    manager.load_plugin(descriptor.plugin_key, true);
    require(manager.wait_for_plugin_load(descriptor.plugin_key, std::chrono::seconds(20), error), error);
    return descriptor;
}
std::vector<GUI::TextureLibraryEntry> plugin_entries(bool rescan = false)
{
    std::vector<GUI::TextureLibraryEntry> out;
    for (const auto& entry : GUI::texture_library(rescan))
        if (entry.source == GUI::TextureSource::Plugin)
            out.push_back(entry);
    return out;
}
}

void test_texture_plugin(const std::string& scenario, const test_fs::path& fixtures)
{
    auto& manager = PluginManager::instance();
    auto& registry = PluginResourceTextures::instance();
    std::string error;
    if (scenario == "texture_bake") {
        const auto input = its_make_cube(8., 8., 8.);
        TextureDisplacementRequest request;
        request.image_data = std::make_shared<std::vector<unsigned char>>(read_bytes(fixtures / "texture-white.png"));
        request.edge_length_mm = 2.;
        request.depth_mm = 0.5f;
        size_t progress_calls = 0;
        const auto result = build_texture_displacement_mesh(input, request, [&](const char*, double) { ++progress_calls; return true; });
        require(result.success() && progress_calls > 4, "upstream TextureBake pipeline did not run: " + result.error);
        require(result.mesh.indices.size() > input.indices.size(), "texture bake did not refine real geometry");
        float zmax = 0.f, zmin = 100.f;
        for (const auto& p : result.mesh.vertices) { zmax = std::max(zmax, p.z()); zmin = std::min(zmin, p.z()); }
        require(zmax > 8.3f && zmin >= -1e-5f, "height map did not displace geometry / plate clamp failed");
        TriangleMesh baked(result.mesh);
        require(baked.volume() > 512., "displaced mesh volume did not change");
        require(its_num_open_edges(result.mesh) == 0, "upstream bake introduced non-manifold geometry");
        const auto second = build_texture_displacement_mesh(input, request);
        require(second.success() && second.mesh.vertices == result.mesh.vertices && second.mesh.indices == result.mesh.indices,
                "preview and commit pipeline output differ for the same snapshot");
        const auto canceled = build_texture_displacement_mesh(input, request, [](const char*, double) { return false; });
        require(canceled.canceled && canceled.mesh.indices.empty(), "canceled bake published geometry");
        size_t callbacks = 0;
        const auto midflight = build_texture_displacement_mesh(input, request, [&](const char*, double) { return ++callbacks < 4; });
        require(midflight.canceled && midflight.mesh.indices.empty() && callbacks >= 4, "mid-pipeline cancellation published geometry");
        auto invalid = request;
        invalid.tiling_scale = 0.f;
        require(!build_texture_displacement_mesh(input, invalid).success(), "invalid texture scale accepted");
        invalid = request;
        invalid.face_excluded.assign(input.indices.size(), 1);
        const auto excluded = build_texture_displacement_mesh(input, invalid);
        require(excluded.success(), excluded.error);
        for (const auto& p : excluded.mesh.vertices)
            require(p.minCoeff() >= -1e-5f && p.maxCoeff() <= 8.00001f, "excluded face displaced");
        return;
    }

    const auto a = install_and_load(fixtures / "texturepacka-1.0.0-py3-none-any.whl");
    const PluginCapabilityId a_id{PluginCapabilityType::ResourceTexture, "Contract Pack A", a.plugin_key};
    require(registry.entries().size() == 1, "load-complete observer did not register texture capability");
    const auto b = install_and_load(fixtures / "texturepackb-1.0.0-py3-none-any.whl");
    const PluginCapabilityId b_id{PluginCapabilityType::ResourceTexture, "Contract Pack B", b.plugin_key};
    require(registry.entries().size() == 2, "second pack was not registered");
    auto entries = plugin_entries();
    require(entries.size() == 2 && entries[0].name == entries[1].name && entries[0].plugin_key != entries[1].plugin_key,
            "same-name texture files lost package provenance");
    const auto old_entry = entries[0];
    auto retained = GUI::load_texture_image_data(old_entry, error);
    require(retained && error.empty(), "could not read selected plugin bytes: " + error);
    const auto original = *retained;
    require(original == read_bytes(fixtures / "texture-gray.png"), "plugin image changed during load");
    const auto decoded = decode_texture_image(*retained, error);
    require(decoded.width == 2 && decoded.height == 2 && decoded.pixels == std::vector<uint8_t>({0, 64, 192, 255}),
            "texture decoding changed grayscale bytes/orientation");
    require(std::abs(decoded.sample(Vec2f(.25f, .25f)) - 127.75f / 255.f) < 1e-5f, "upstream bilinear sampler differs");
    require(decoded.sample(Vec2f(-.1f, 0), false) == 0.f, "untiled sample leaked beyond texture bounds");

    if (scenario == "texture_paths") {
        const auto invalid = install_and_load(fixtures / "textureinvalid-1.0.0-py3-none-any.whl");
        require(manager.is_plugin_loaded(invalid.plugin_key) && registry.entries().size() == 2, "invalid directory registered");
        const auto image = test_fs::u8path(old_entry.path);
        const auto outside = test_fs::u8path(data_dir()) / "outside.png";
        test_fs::copy_file(fixtures / "texture-white.png", outside);
        test_fs::remove(image);
        test_fs::create_symlink(outside, image);
        require(!GUI::load_texture_image_data(old_entry, error) && !error.empty(), "stale symlink escaped plugin boundary");
        require(plugin_entries(true).size() == 1, "scanner exposed symlink outside package");
        test_fs::remove(image);
        test_fs::copy_file(fixtures / "texture-gray.png", image);
        for (int i = 0; i < 70; ++i)
            test_fs::copy_file(image, image.parent_path() / ("extra" + std::to_string(i) + ".png"));
        entries = plugin_entries(true);
        require(entries.size() == 65, "64-image package bound was not applied");
        const auto a_entry = *std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.plugin_key == a.plugin_key; });
        std::ofstream(test_fs::u8path(a_entry.path), std::ios::binary | std::ios::trunc) << "not a png";
        wxInitAllImageHandlers();
        require(!GUI::load_texture_image_data(a_entry, error) && !error.empty(), "corrupt image accepted");
        wxImage::CleanUpHandlers();
        require(*retained == original, "failed or replaced resource mutated retained project bytes");
        return;
    }
    if (scenario == "texture_library") {
        wxInitAllImageHandlers();
        const auto imported = GUI::import_texture_to_library((fixtures / "texture-rgb.png").u8string(), error);
        require(imported && imported->source == GUI::TextureSource::User, "import failed: " + error);
        const auto rgb_bytes = GUI::load_texture_image_data(*imported, error);
        require(bool(rgb_bytes), error);
        const auto rgb = decode_texture_image(*rgb_bytes, error);
        require(rgb.has_color() && rgb.rgb == std::vector<uint8_t>({255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255}),
                "RGB channels or row orientation changed");
        const auto copy = GUI::save_texture_to_library(decoded, "../../texture-safe", error);
        require(copy && test_fs::u8path(copy->path).parent_path() == test_fs::u8path(GUI::user_texture_dir()),
                "saved texture escaped user library");
        require(!GUI::remove_user_texture(old_entry.path, error), "removed read-only plugin texture");
        require(GUI::remove_user_texture(imported->path, error), error);
        require(!test_fs::exists(test_fs::u8path(imported->path)) && !rgb_bytes->empty(), "imported bytes did not outlive deletion");
        require(GUI::remove_user_texture(copy->path, error), error);
        wxImage::CleanUpHandlers();
        return;
    }

    const auto generation = GUI::texture_library_generation();
    require(manager.set_capability_enabled(a_id, false), "disable capability failed");
    require(registry.entries().size() == 1 && plugin_entries().size() == 1 && GUI::texture_library_generation() > generation,
            "disable did not invalidate texture library");
    require(!GUI::load_texture_image_data(old_entry, error), "stale picker entry could read disabled capability");
    require(!GUI::load_texture_image_data(old_entry.path, error), "path overload could read a disabled texture pack");
    require(*retained == original, "disabling pack invalidated applied texture bytes");
    require(manager.unload_plugin(a.plugin_key), "unload disabled texture pack failed");
    manager.load_plugin(a.plugin_key, true);
    require(manager.wait_for_plugin_load(a.plugin_key, std::chrono::seconds(20), error), error);
    require(registry.entries().size() == 1 && plugin_entries().size() == 1, "reload forgot capability disabled state");
    require(manager.set_capability_enabled(a_id, true) && plugin_entries().size() == 2, "enable did not repopulate library");
    require(manager.unload_plugin(a.plugin_key), "unload texture pack failed");
    require(plugin_entries().size() == 1 && !GUI::load_texture_image_data(old_entry, error), "unload left usable stale resources");
    test_fs::copy_file(fixtures / "texture-white.png", test_fs::u8path(old_entry.path), test_fs::copy_options::overwrite_existing);
    manager.load_plugin(a.plugin_key, true);
    require(manager.wait_for_plugin_load(a.plugin_key, std::chrono::seconds(20), error), error);
    entries = plugin_entries();
    require(entries.size() == 2, "reload did not refresh library");
    auto reloaded = GUI::load_texture_image_data(entries[0], error);
    require(reloaded && *reloaded != original && *retained == original, "same-path reload reused stale image bytes");
    require(manager.delete_plugin(a.plugin_key, error), error);
    require(plugin_entries().size() == 1 && !GUI::load_texture_image_data(old_entry, error), "uninstall left usable resources");
    require(!decode_texture_image(*retained, error).empty(), "project texture did not outlive uninstall");
    require(manager.get_plugin_capability(b_id)->ref_count() == 0, "registry retained a Python consumer pin");
    manager.shutdown();
    require(registry.entries().empty() && !decode_texture_image(*retained, error).empty(), "shutdown retained Python entries or lost bytes");
    PythonInterpreter::instance().shutdown();
    TextureDisplacementRequest detached;
    detached.image_data = retained;
    detached.edge_length_mm = 2.;
    require(build_texture_displacement_mesh(its_make_cube(8., 8., 8.), detached).success(),
            "applied texture could not be consumed after Python finalized");
}
