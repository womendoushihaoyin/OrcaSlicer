#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/log/core.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace test_fs = std::filesystem;
using namespace Slic3r;
using json = nlohmann::json;

namespace {
void check(bool value, const std::string& message)
{
    if (!value)
        throw std::runtime_error(message);
}
void write(const test_fs::path& file, const json& value)
{
    test_fs::create_directories(file.parent_path());
    boost::nowide::ofstream out(file.u8string());
    out << value.dump(2);
    check(bool(out), "Cannot write fixture");
}
json process(const std::string& name, double height, const std::string& parent = {})
{
    return {{"type", "process"}, {"name", name}, {"version", "2.4.1.0"},
            {"layer_height", std::to_string(height)}, {"inherits", parent}};
}
double height(const Preset& preset) { return preset.config.option<ConfigOptionFloat>("layer_height")->value; }
std::map<std::string, std::string> contents(const test_fs::path& directory)
{
    std::map<std::string, std::string> result;
    for (const auto& entry : test_fs::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file())
            continue;
        boost::nowide::ifstream input(entry.path().u8string(), std::ios::binary);
        result.emplace(entry.path().lexically_relative(directory).u8string(),
                       std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
    }
    return result;
}
void run(const test_fs::path& profile)
{
    const auto directory = profile / "user/default/_local" / test_fs::u8path("本地包");
    const std::string id = "本地包";
    const std::string prefix = "_local/" + id + "/";
    write(directory / "bundle_metadata.json", {{"id", id}, {"name", "Local contract"}, {"version", "1.0"}});
    write(directory / "process/base/Z Parent.json", process("Z Parent", 0.2));
    write(directory / "process/A Child.json", process("A Child", 0.22, "Z Parent"));
    write(directory / "machine/Local Printer.json", {{"type", "machine"}, {"name", "Local Printer"}, {"version", "2.4.1.0"}});
    write(directory / "filament/Local Filament.json", {{"type", "filament"}, {"name", "Local Filament"}, {"version", "2.4.1.0"},
          {"compatible_printers", json::array({"Local Printer"})}});
    write(profile / "user/default/_local/bad/bundle_metadata.json", {{"id", "different"}});
    PresetBundle bundle;
    bundle.filament_presets = {bundle.filaments.get_selected_preset_name()};
    std::string error;
    check(bundle.list_local_bundle_ids("") == std::vector<std::string>{id}, "Dynamic bundle list/metadata filtering failed");
    check(!bundle.reload_local_bundle("default", "../escape", &error), "Path traversal accepted");
    check(!bundle.reload_local_bundle("../escape", id, &error), "User directory traversal accepted");
    check(bundle.reload_local_bundle("", id, &error), "Initial reload failed: " + error);
    auto* child = bundle.prints.find_preset(prefix + "A Child", false, true);
    check(child && child->bundle_id == id && child->is_from_bundle() && !child->is_user(), "Actual bundle provenance missing");
    check(child->inherits() == prefix + "Z Parent", "Internal inheritance did not use canonical names");
    auto* filament = bundle.filaments.find_preset(prefix + "Local Filament", false);
    check(filament && filament->config.option<ConfigOptionStrings>("compatible_printers")->values ==
          std::vector<std::string>{prefix + "Local Printer"}, "Internal compatibility references were not canonicalized");
    check(bundle.local_bundle_metadata.at(id).print_presets.size() == 2, "Metadata cache was not committed");

    bundle.prints.select_preset_by_name(child->name, true);
    bundle.prints.get_edited_preset().config.option<ConfigOptionFloat>("layer_height")->value = 0.25;
    bundle.prints.get_edited_preset().config.option<ConfigOptionString>("mixed_filament_definitions", true)->value = "local reload must preserve edits";
    bundle.prints.update_dirty();
    bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = {0., 123., 321., 0.};
    const auto project_before = bundle.project_config;
    write(directory / "process/A Child.json", process("A Child", 0.3, "Z Parent"));
    check(bundle.reload_local_bundle("", id, &error), "Dirty-preserving reload failed: " + error);
    check(height(bundle.prints.get_edited_preset()) == 0.25 && height(bundle.prints.get_selected_preset()) == 0.3,
          "Reload did not preserve edited options over new base");
    check(bundle.prints.find_preset(prefix + "A Child", false, true) == child, "Same-layout reload invalidated a Preset reference");
    check(bundle.project_config.diff(project_before).empty(), "Reload changed unrelated project/purge/mixed config");
    check(bundle.prints.get_edited_preset().config.option<ConfigOptionString>("mixed_filament_definitions")->value ==
          "local reload must preserve edits", "Reload regenerated mixed filament definitions");

    const auto before = bundle.prints.size();
    {
        boost::nowide::ofstream broken((directory / "process/A Child.json").u8string());
        broken << "{ invalid json";
    }
    check(!bundle.reload_local_bundle("", id, &error), "Invalid JSON was accepted");
    check(bundle.prints.size() == before && height(bundle.prints.get_selected_preset()) == 0.3 &&
          height(bundle.prints.get_edited_preset()) == 0.25, "Failed reload mutated live collection");
    check(test_fs::is_regular_file(directory / "process/A Child.json"), "Failed parse deleted source JSON");
    write(directory / "process/A Child.json", process("A Child", 0.3, "Z Parent"));
    write(directory / "process/base/Z Parent.json", process("Z Parent", 0.2, "A Child"));
    check(!bundle.reload_local_bundle("", id, &error), "Inheritance cycle was accepted");
    write(directory / "process/base/Z Parent.json", process("Z Parent", 0.2));

    test_fs::remove(directory / "process/A Child.json");
    check(!bundle.reload_local_bundle("", id, &error), "Removing a dirty selected preset was accepted");
    check(height(bundle.prints.get_edited_preset()) == 0.25, "Dirty selection lost after refusal");
    write(directory / "process/A Child.json", process("A Child", 0.3, "Z Parent"));

    auto external_config = bundle.prints.default_preset().config;
    external_config.option<ConfigOptionString>("inherits", true)->value = prefix + "Z Parent";
    bundle.prints.load_preset("", "Outside Child", std::move(external_config), false);
    check(!bundle.reload_local_bundle("", id, &error), "Reload ignored an external inheritance dependency");
    check(bundle.prints.find_preset("Outside Child", false), "External preset lost after refusal");

    test_fs::create_directories(profile / "user/default/_subscribed" / test_fs::u8path(id));
    check(bundle.list_local_bundle_ids("").empty(), "Subscribed ID collision listed as reloadable");
    check(!bundle.reload_local_bundle("", id, &error), "Subscribed ID collision accepted");
    test_fs::remove_all(profile / "user/default/_subscribed");

    PresetBundle startup;
    startup.load_user_presets("default", ForwardCompatibilitySubstitutionRule::Enable);
    check(startup.prints.find_preset(prefix + "A Child", false), "Startup did not load local bundles");
    check(startup.filaments.find_preset(prefix + "Local Filament", false), "Startup lost filament bundle");

    const auto original_files = contents(directory);
    const auto source_name = prefix + "A Child";
    startup.prints.select_preset_by_name(source_name, true);
    auto& edited = startup.prints.get_edited_preset();
    edited.config.option<ConfigOptionFloat>("layer_height")->value = 0.27;
    edited.save(nullptr);
    edited.save_info();
    edited.save_info((directory / "forbidden.info").u8string());
    edited.remove_files();
    startup.prints.save_current_preset(source_name, false, false);
    check(height(startup.prints.get_selected_preset()) == 0.3, "In-place bundle save modified the live source");
    check(!startup.prints.delete_current_preset(), "Selected bundle preset was deleted");
    check(!startup.prints.delete_preset(source_name), "Named bundle preset was deleted");
    std::vector<std::string> failures;
    auto* source = startup.prints.find_preset(source_name, false, true);
    check(!startup.prints.clone_presets({source}, failures, [](Preset&, Preset::Type&) {}, true),
          "Forced clone overwrote a bundle preset");
    check(failures == std::vector<std::string>{source_name}, "Protected clone did not identify the collision");
    check(contents(directory) == original_files, "Save/delete API changed original bundle files");

    startup.prints.save_current_preset("User Copy", false, false);
    auto& user_copy = startup.prints.get_selected_preset();
    check(user_copy.is_user() && user_copy.bundle_id.empty() && user_copy.inherits() == source_name,
          "Save As did not create a user preset inheriting its bundle source");
    check(height(user_copy) == 0.27 && test_fs::is_regular_file(test_fs::u8path(user_copy.file)), "Save As did not persist the edited config");
    check(test_fs::u8path(user_copy.file).parent_path() == profile / "user/default/process", "Save As used the bundle directory");
    startup.prints.select_preset_by_name(source_name, true);
    startup.prints.save_current_preset("Detached Copy", true, false);
    check(startup.prints.get_selected_preset().is_user() && startup.prints.get_selected_preset().bundle_id.empty() &&
          startup.prints.get_selected_preset().inherits().empty(), "Detached Save As retained source provenance");
    failures.clear();
    source = startup.prints.find_preset(source_name, false, true);
    check(startup.prints.clone_presets({source}, failures, [](Preset& preset, Preset::Type&) { preset.name = "Cloned Copy"; }, false),
          "Bundle clone into user preset failed");
    const auto* cloned = startup.prints.find_preset("Cloned Copy", false, true);
    check(cloned && cloned->is_user() && cloned->bundle_id.empty() && test_fs::is_regular_file(test_fs::u8path(cloned->file)),
          "Clone retained bundle provenance or was not persisted");
    check(contents(directory) == original_files, "Save As/clone changed original bundle files");
    PresetBundle restarted;
    restarted.load_user_presets("default", ForwardCompatibilitySubstitutionRule::Enable);
    const auto* persisted = restarted.prints.find_preset("User Copy", false, true);
    check(persisted && persisted->is_user() && persisted->bundle_id.empty() && persisted->inherits() == source_name && height(*persisted) == 0.27,
          "Restart did not restore the user copy and its local bundle parent");
    restarted.prints.select_preset_by_name("Cloned Copy", true);
    const auto clone_file = test_fs::u8path(restarted.prints.get_selected_preset().file);
    check(restarted.prints.delete_current_preset() && !test_fs::exists(clone_file), "User copy deletion was blocked");
    restarted.reset(true);
    check(contents(directory) == original_files, "Collection reset deleted bundle source files");
    std::cout << "PASS local bundle listing, provenance, inheritance, compatibility, dirty reload, failure rollback, source preservation, external dependencies, startup, save/delete protection, user Save As/clone/restart\n";
}
} // namespace

int main(int argc, char** argv)
{
    boost::nowide::args utf8_args(argc, argv);
    boost::nowide::nowide_filesystem();
    if (argc != 2)
        return 2;
    const auto profile = test_fs::u8path(argv[1]);
    if (test_fs::exists(profile))
        return 3;
    test_fs::create_directories(profile);
    set_data_dir(profile.u8string());
    boost::log::core::get()->set_logging_enabled(false);
    try {
        run(profile);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL local bundles: " << error.what() << '\n';
        return 1;
    }
}
