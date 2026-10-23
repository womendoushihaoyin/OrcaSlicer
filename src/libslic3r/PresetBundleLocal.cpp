#include "PresetBundle.hpp"
#include "Utils.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
constexpr const char* PRESET_LOCAL_DIR = "_local";
constexpr const char* PRESET_SUBSCRIBED_DIR = "_subscribed";
constexpr const char* PRESET_BUNDLE_METADATA = "bundle_metadata.json";

bool is_single_path_component(const std::string& value)
{
    return !value.empty() && value != "." && value != ".." && value.find_first_of("/\\:") == std::string::npos &&
           !fs::u8path(value).has_root_path();
}

bool check_local_bundle(const fs::path& user_dir, const std::string& id, LocalPresetBundleMetadata& metadata, std::string& error)
{
    if (!is_single_path_component(id)) {
        error = "Local bundle id must be a single path component";
        return false;
    }
    const auto directory = user_dir / PRESET_LOCAL_DIR / fs::u8path(id);
    const auto file = directory / PRESET_BUNDLE_METADATA;
    if (!fs::is_directory(directory) || !fs::is_regular_file(file) || fs::is_symlink(user_dir) || fs::is_symlink(user_dir / PRESET_LOCAL_DIR) ||
        fs::is_symlink(directory) || fs::is_symlink(file)) {
        error = "Local bundle must be a real directory with regular metadata";
        return false;
    }
    if (fs::exists(user_dir / PRESET_SUBSCRIBED_DIR / fs::u8path(id))) {
        error = "A subscribed bundle already uses this id";
        return false;
    }
    try {
        boost::nowide::ifstream input(file.u8string());
        json value;
        input >> value;
        metadata.id = value.at("id").get<std::string>();
        if (metadata.id != id)
            throw std::runtime_error("Metadata id does not match directory");
        metadata.name = value.value("name", value.value("bundle_id", id));
        metadata.version = value.value("version", std::string());
        metadata.description = value.value("description", value.value("bundle_type", std::string()));
        metadata.author = value.value("author", std::string());
        metadata.imported_time = value.value("imported_time", 0LL);
        metadata.updated_time = value.value("updated_time", 0LL);
        metadata.path = file.u8string();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Invalid local bundle metadata: ") + exception.what();
        return false;
    }
}

struct ParsedPreset
{
    std::string bare_name;
    std::string file;
    DynamicPrintConfig config;
    std::map<std::string, std::string> attributes;
    Semver version;
    bool visiting = false;
    bool loaded = false;
};
using ParsedCollection = std::map<std::string, ParsedPreset>;

ParsedCollection parse_collection(const fs::path& directory, Preset::Type type)
{
    ParsedCollection result;
    if (!fs::exists(directory))
        return result;
    if (!fs::is_directory(directory) || fs::is_symlink(directory))
        throw std::runtime_error("Bundle preset directory must not be a symbolic link");
    auto parse_directory = [&](const fs::path& folder) {
        if (!fs::exists(folder))
            return;
        if (!fs::is_directory(folder) || fs::is_symlink(folder))
            throw std::runtime_error("Bundle base directory must not be a symbolic link");
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (entry.path().extension() != ".json")
                continue;
            if (!entry.is_regular_file() || entry.is_symlink())
                throw std::runtime_error("Bundle preset must be a regular JSON file");
            ParsedPreset preset;
            preset.file = entry.path().u8string();
            std::string reason;
            preset.config.load_from_json(preset.file, ForwardCompatibilitySubstitutionRule::Enable, preset.attributes, reason);
            if (!reason.empty())
                throw std::runtime_error("Invalid bundle preset: " + preset.file + ": " + reason);
            auto& values = preset.attributes;
            if (values.count("type") && Preset::get_type_from_string(values.at("type")) != type)
                throw std::runtime_error("Bundle preset type does not match its directory");
            preset.bare_name = values.count("name") ? values.at("name") : entry.path().stem().u8string();
            if (!is_single_path_component(preset.bare_name))
                throw std::runtime_error("Bundle preset name must be a single path component");
            auto version = Semver::parse(values["version"]);
            if (!version)
                throw std::runtime_error("Bundle preset has an invalid version");
            preset.version = *version;
            const auto key = preset.bare_name;
            if (!result.emplace(key, std::move(preset)).second)
                throw std::runtime_error("Duplicate preset name in local bundle: " + key);
        }
    };
    parse_directory(directory / "base");
    parse_directory(directory);
    return result;
}

void load_bundle_presets(PresetBundle& bundle, const fs::path& directory, LocalPresetBundleMetadata& metadata)
{
    std::array<ParsedCollection, 3> parsed = {
        parse_collection(directory / PRESET_PRINT_NAME, Preset::TYPE_PRINT),
        parse_collection(directory / PRESET_FILAMENT_NAME, Preset::TYPE_FILAMENT),
        parse_collection(directory / PRESET_PRINTER_NAME, Preset::TYPE_PRINTER)};
    std::array<PresetCollection*, 3> collections = {&bundle.prints, &bundle.filaments, &bundle.printers};
    std::array<std::vector<std::string>*, 3> names = {&metadata.print_presets, &metadata.filament_presets, &metadata.printer_presets};
    const std::string prefix = std::string(PRESET_LOCAL_DIR) + "/" + metadata.id + "/";
    for (size_t index = 0; index < parsed.size(); ++index) {
        auto& collection = *collections[index];
        std::function<void(ParsedPreset&)> load = [&](ParsedPreset& parsed_preset) {
            if (parsed_preset.loaded)
                return;
            if (parsed_preset.visiting)
                throw std::runtime_error("Local bundle contains an inheritance cycle");
            parsed_preset.visiting = true;
            auto config = parsed_preset.config;
            auto* inherits = config.option<ConfigOptionString>("inherits");
            Preset* parent = nullptr;
            if (inherits && !inherits->value.empty()) {
                auto internal = parsed[index].find(inherits->value);
                if (internal != parsed[index].end()) {
                    load(internal->second);
                    inherits->value = prefix + internal->first;
                }
                parent = collection.find_preset2(inherits->value, false);
                if (!parent)
                    throw std::runtime_error("Local bundle parent preset was not found: " + inherits->value);
            }
            for (const auto& reference : {std::make_pair("compatible_prints", size_t(0)), std::make_pair("compatible_printers", size_t(2))}) {
                if (auto* values = config.option<ConfigOptionStrings>(reference.first))
                    for (auto& name : values->values)
                        if (parsed[reference.second].count(name))
                            name = prefix + name;
            }
            const auto& defaults = collection.default_preset_for(config);
            DynamicPrintConfig complete = parent ? parent->config : defaults.config;
            complete.apply(config);
            Preset::normalize(complete);
            const auto invalid_keys = Preset::remove_invalid_keys(complete, defaults.config);
            if (!invalid_keys.empty())
                throw std::runtime_error("Invalid keys in local bundle preset: " + invalid_keys);
            const auto canonical_name = prefix + parsed_preset.bare_name;
            if (collection.find_preset(canonical_name, false))
                throw std::runtime_error("Local bundle collides with an existing preset: " + canonical_name);
            // Do not retain a pointer into the deque across load_preset insertion.
            const auto inherited_filament_id = parent ? parent->filament_id : std::string();
            auto& preset = collection.load_preset(parsed_preset.file, canonical_name, std::move(complete), false, parsed_preset.version,
                                                   parsed_preset.attributes["is_custom_defined"] == "1");
            preset.bundle_id = metadata.id;
            preset.alias = parsed_preset.bare_name;
            preset.filament_id = parsed_preset.attributes.count("filament_id") ? parsed_preset.attributes["filament_id"] : inherited_filament_id;
            preset.description = parsed_preset.attributes["description"];
            preset.is_visible = parsed_preset.attributes["instantiation"] != "false";
            if (index == 0)
                preset.config.option<ConfigOptionString>("print_settings_id", true)->value = canonical_name;
            else if (index == 1)
                preset.config.option<ConfigOptionStrings>("filament_settings_id", true)->values = {canonical_name};
            else
                preset.config.option<ConfigOptionString>("printer_settings_id", true)->value = canonical_name;
            names[index]->push_back(canonical_name);
            parsed_preset.loaded = true;
            parsed_preset.visiting = false;
        };
        for (auto& entry : parsed[index])
            load(entry.second);
    }
}
} // namespace

std::vector<std::string> PresetBundle::list_local_bundle_ids(const std::string& preset_folder) const
{
    std::vector<std::string> ids;
    const std::string user = preset_folder.empty() ? DEFAULT_USER_FOLDER_NAME : preset_folder;
    if (!is_single_path_component(user))
        return ids;
    const auto user_dir = std::filesystem::u8path(data_dir()) / PRESET_USER_DIR / std::filesystem::u8path(user);
    const auto directory = user_dir / "_local";
    try {
        if (!std::filesystem::is_directory(directory) || std::filesystem::is_symlink(directory))
            return ids;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            LocalPresetBundleMetadata metadata;
            std::string error;
            const auto id = entry.path().filename().u8string();
            if (check_local_bundle(user_dir, id, metadata, error))
                ids.push_back(id);
        }
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(warning) << "Local bundle listing: " << error.what();
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void PresetBundle::load_local_bundles(const std::string& preset_folder)
{
    for (const auto& id : list_local_bundle_ids(preset_folder)) {
        std::string error;
        if (!reload_local_bundle(preset_folder, id, &error))
            BOOST_LOG_TRIVIAL(warning) << "Cannot load local bundle " << id << ": " << error;
    }
}

// Reload transaction adapted from pinned Orca 14a1aa333; parsing is read-only.
bool PresetBundle::reload_local_bundle(const std::string& preset_folder, const std::string& bundle_id, std::string* error)
{
    auto fail = [error](const std::string& message) {
        if (error != nullptr)
            *error = message;
        return false;
    };
    if (!is_single_path_component(bundle_id))
        return fail("Local bundle id must be a single path component");

    const std::string user = preset_folder.empty() ? DEFAULT_USER_FOLDER_NAME : preset_folder;
    if (!is_single_path_component(user))
        return fail("Preset folder must be a single path component");

    try {
        const fs::path user_dir      = fs::u8path(data_dir()) / PRESET_USER_DIR / fs::u8path(user);
        const fs::path bundle_dir    = user_dir / PRESET_LOCAL_DIR / fs::u8path(bundle_id);
        const fs::path metadata_file = bundle_dir / PRESET_BUNDLE_METADATA;

        LocalPresetBundleMetadata metadata;
        std::string    check_error;
        if (!check_local_bundle(user_dir, bundle_id, metadata, check_error))
            return fail(check_error);

        // Keep background preset sync from changing the collections between copying and committing them.
        std::scoped_lock collection_lock(prints.m_mutex, filaments.m_mutex, printers.m_mutex);
        const std::string target_prefix = std::string(PRESET_LOCAL_DIR) + "/" + bundle_id + "/";
        const auto is_target = [&](const Preset& preset) {
            return preset.bundle_id == bundle_id && boost::starts_with(preset.name, target_prefix);
        };
        struct Selection {
            std::string              name;
            Preset                   base{Preset::TYPE_INVALID, std::string()};
            Preset                   edited{Preset::TYPE_INVALID, std::string()};
            std::vector<std::string> dirty_options;
        };
        auto snapshot = [&](const PresetCollection& collection, Selection& selection) {
            selection.name = collection.get_selected_preset_name();
            if (selection.name.empty())
                return false;
            for (const Preset& preset : collection.get_presets()) {
                if (preset.name == selection.name) {
                    selection.base = preset;
                    selection.edited = collection.get_edited_preset();
                    selection.dirty_options = PresetCollection::dirty_options(&selection.edited, &selection.base);
                    return true;
                }
            }
            return false;
        };

        Selection print_selection, filament_selection, printer_selection;
        if (!snapshot(prints, print_selection) || !snapshot(filaments, filament_selection) || !snapshot(printers, printer_selection))
            return fail("Preset selections are not initialized");

        PresetBundle staged;
        // Collection copies keep vendor pointers into the live vendor map, which is not replaced by this reload.
        staged.prints           = prints;
        staged.filaments        = filaments;
        staged.printers         = printers;
        staged.filament_presets = filament_presets;
        auto remove_target = [&](PresetCollection& collection) {
            auto begin = collection.m_presets.begin() + collection.m_num_default_presets;
            collection.m_presets.erase(std::remove_if(begin, collection.m_presets.end(), is_target), collection.m_presets.end());
            std::sort(collection.m_presets.begin() + collection.m_num_default_presets, collection.m_presets.end());
            // load_preset inserts relative to the selected index. Removing the
            // previous bundle may invalidate it; the snapshot is restored later.
            collection.select_preset(0);
        };
        remove_target(staged.prints);
        remove_target(staged.filaments);
        remove_target(staged.printers);

        load_bundle_presets(staged, bundle_dir, metadata);

        auto dirty_target_was_removed = [&](const Selection& selection, const PresetCollection& collection) {
            if (selection.dirty_options.empty() || !is_target(selection.base))
                return false;
            for (const Preset& preset : collection.get_presets())
                if (preset.name == selection.name && is_target(preset))
                    return false;
            return true;
        };
        if (dirty_target_was_removed(print_selection, staged.prints) ||
            dirty_target_was_removed(filament_selection, staged.filaments) ||
            dirty_target_was_removed(printer_selection, staged.printers))
            return fail("A modified selected preset was removed or renamed");

        auto external_child_name = [&](const PresetCollection& collection) {
            auto depends_on_target = [&](const Preset& preset) {
                if (is_target(preset))
                    return false;
                const Preset* parent = &preset;
                std::unordered_set<const Preset*> visited;
                while ((parent = collection.get_preset_parent(*parent)) != nullptr && visited.insert(parent).second)
                    if (is_target(*parent))
                        return true;
                for (const char* key : {"compatible_prints", "compatible_printers"}) {
                    const auto* names = preset.config.option<ConfigOptionStrings>(key);
                    if (names == nullptr)
                        continue;
                    const PresetCollection& referenced = std::string(key) == "compatible_printers" ? printers : prints;
                    for (const std::string& name : names->values) {
                        const Preset* dependency = referenced.find_preset(name, false);
                        if (dependency != nullptr && is_target(*dependency))
                            return true;
                    }
                }
                return false;
            };
            for (const Preset& preset : collection.get_presets())
                if (depends_on_target(preset))
                    return preset.name;
            if (depends_on_target(collection.get_edited_preset()))
                return collection.get_edited_preset().name;
            return std::string();
        };
        for (const PresetCollection* collection : std::array<const PresetCollection*, 3>{&prints, &filaments, &printers}) {
            const std::string child_name = external_child_name(*collection);
            if (!child_name.empty())
                return fail("Preset outside this bundle depends on it: " + child_name);
        }

        auto restore_selection = [](PresetCollection& collection, const Selection& selection) {
            if (collection.find_preset(selection.name, false, true) == nullptr) {
                collection.select_preset(collection.first_visible_idx());
                return false;
            }
            collection.select_preset_by_name(selection.name, true);
            if (collection.get_selected_preset_name() != selection.name)
                return false;
            Preset& edited = collection.get_edited_preset();
            for (const std::string& key : selection.dirty_options) {
                if (const ConfigOption* option = selection.edited.config.option(key))
                    edited.config.set_key_value(key, option->clone());
                else
                    edited.config.erase(key);
            }
            collection.update_dirty();
            return true;
        };

        staged.prints.update_map_alias_to_profile_name();
        staged.filaments.update_map_alias_to_profile_name();
        staged.printers.update_map_alias_to_profile_name();
        const bool printer_preserved  = restore_selection(staged.printers, printer_selection);
        const bool print_preserved    = restore_selection(staged.prints, print_selection);
        const bool filament_preserved = restore_selection(staged.filaments, filament_selection);
        const auto selection_mode = [printer_preserved](bool selection_preserved) {
            if (!selection_preserved)
                return PresetSelectCompatibleType::Always;
            return printer_preserved ? PresetSelectCompatibleType::Never : PresetSelectCompatibleType::OnlyIfWasCompatible;
        };
        if (staged.printers.get_edited_preset().printer_technology() != ptFFF)
            return fail("Local bundle reload requires an FFF printer selection");
        staged.update_compatible(selection_mode(print_preserved), selection_mode(filament_preserved));
        for (std::string& name : staged.filament_presets)
            if (boost::starts_with(name, target_prefix) && staged.filaments.find_preset(name, false) == nullptr)
                name = staged.filaments.first_compatible().name;
        // Slot count does not change during reload. Preserve the project's purge
        // matrix, colours and mixed-filament definitions. This branch's
        // update_multi_material_filament_presets also writes mixed definitions
        // into the edited print preset, so it is not a pure purge-size update.

        struct Collection
        {
            PresetCollection& live;
            PresetCollection& candidate;
            size_t original_size;
        };
        std::array<Collection, 3> collections{{{prints, staged.prints, prints.size()},
                                               {filaments, staged.filaments, filaments.size()},
                                               {printers, staged.printers, printers.size()}}};
        for (size_t i = 0; i < collections.size(); ++i) {
            const auto& collection   = collections[i];
            const auto dirty_options = collection.live.current_dirty_options();
            if (dirty_options.empty())
                continue;
            if (collection.live.get_selected_preset_name() != collection.candidate.get_selected_preset_name())
                return fail("Reload would discard a modified selected preset");
            for (const std::string& key : dirty_options) {
                const ConfigOption* before = collection.live.get_edited_preset().config.option(key);
                const ConfigOption* after  = collection.candidate.get_edited_preset().config.option(key);
                if ((before == nullptr) != (after == nullptr) || (before != nullptr && *before != *after))
                    return fail("Reload would discard a modified preset option: " + key);
            }
        }

        if (calibrate_printer != nullptr && staged.printers.find_preset(calibrate_printer->name, false, true) != nullptr)
            staged.set_calibrate_printer(calibrate_printer->name);
        metadata.path = metadata_file.u8string();
        std::set<const Preset*> calibration_filaments;
        const Preset* calibration_printer = nullptr;
        try {
            // Growing a deque preserves existing objects. Roll back appended copies if preparation fails.
            for (size_t i = 0; i < collections.size(); ++i) {
                auto& collection = collections[i];
                while (collection.live.size() < collection.candidate.size())
                    collection.live.m_presets.emplace_back(collection.candidate.m_presets[collection.live.size()]);
            }
            auto calibration_preset = [](const Preset* preset, PresetCollection& live, PresetCollection& candidate) -> const Preset* {
                if (preset == nullptr)
                    return nullptr;
                if (preset == &candidate.get_edited_preset())
                    return &live.get_edited_preset();
                const auto it = candidate.find_preset_internal(preset->name);
                return &live.m_presets[it - candidate.m_presets.begin()];
            };
            calibration_printer = calibration_preset(staged.calibrate_printer, printers, staged.printers);
            for (const Preset* preset : staged.calibrate_filaments)
                calibration_filaments.insert(calibration_preset(preset, filaments, staged.filaments));
            // Insertion is the last allocating operation. Replacing metadata only moves its members.
            local_bundle_metadata.insert_or_assign(bundle_id, std::move(metadata));
        } catch (...) {
            for (size_t i = 0; i < collections.size(); ++i)
                while (collections[i].live.size() > collections[i].original_size)
                    collections[i].live.m_presets.pop_back();
            throw;
        }
        auto install_target = [](PresetCollection& live, PresetCollection& candidate) noexcept {
            // Keep stored Preset references alive when the collection's layout is unchanged.
            // Adding/removing presets retains the collection's usual sorted-position semantics.
            for (size_t i = 0; i < candidate.size(); ++i)
                live.m_presets[i] = std::move(candidate.m_presets[i]);
            while (live.size() > candidate.size())
                live.m_presets.pop_back();
            live.m_map_alias_to_profile_name.swap(candidate.m_map_alias_to_profile_name);
            // Move assignment does not allocate (including Semver); swapping Presets would construct temporary containers.
            live.m_edited_preset = std::move(candidate.m_edited_preset);
            live.m_saved_preset  = std::move(candidate.m_saved_preset);
            live.m_idx_selected  = candidate.m_idx_selected;
        };
        for (size_t i = 0; i < collections.size(); ++i)
            install_target(collections[i].live, collections[i].candidate);
        filament_presets.swap(staged.filament_presets);
        calibrate_printer = calibration_printer;
        calibrate_filaments.swap(calibration_filaments);
        return true;
    } catch (const std::exception& exception) {
        return fail(std::string("Failed to reload local bundle: ") + exception.what());
    }
}

} // namespace Slic3r
