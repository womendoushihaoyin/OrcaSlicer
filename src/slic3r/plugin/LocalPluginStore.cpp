#include "LocalPluginStore.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace Slic3r {
namespace {

constexpr const char* state_file = ".install_state.json";

bool valid_key(const std::string& key)
{
    if (key.empty() || key.front() == '.' || key.rfind("__", 0) == 0 || key == "_subscribed" || key == "plugin_data")
        return false;
    return std::all_of(key.begin(), key.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

std::string suffix()
{
    static std::atomic<unsigned> serial{0};
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial);
}

bool ignored(const fs::path& path)
{
    const auto name = path.filename().u8string();
    return name.empty() || name.front() == '.' || name.rfind("__", 0) == 0 || name == "_subscribed" || name == "plugin_data";
}

// Serializes filesystem mutations by cooperating app instances. A lock left by
// an interrupted process is reported for recovery, never silently stolen.
class StoreLock
{
public:
    explicit StoreLock(const fs::path& root) : m_path(root / ".local-install.lock")
    {
        if (!fs::create_directory(m_path))
            throw std::runtime_error("Plugin store is busy or has an interrupted operation: " + m_path.u8string());
    }
    ~StoreLock()
    {
        std::error_code ec;
        fs::remove(m_path, ec);
    }
    StoreLock(const StoreLock&)            = delete;
    StoreLock& operator=(const StoreLock&) = delete;

private:
    fs::path m_path;
};

json read_state(const fs::path& dir)
{
    const auto path = dir / state_file;
    if (fs::is_symlink(fs::symlink_status(path)) || !fs::is_regular_file(path))
        throw std::runtime_error("Missing or unsafe plugin install state");
    std::ifstream input(path);
    auto          state = json::parse(input);
    if (!state.is_object() || !state.contains("enabled") || !state["enabled"].is_boolean())
        throw std::runtime_error("Invalid plugin install state: enabled must be a boolean");
    if (state.value("installed_from", std::string{}) != "local" || !state.value("cloud_uuid", std::string{}).empty())
        throw std::runtime_error("This local backend cannot manage cloud packages");
    return state;
}

void write_state(const fs::path& dir, const json& state)
{
    const auto destination = dir / state_file;
    const auto temporary   = dir / (std::string(state_file) + ".tmp-" + suffix());
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << state.dump(2) << '\n';
        output.close();
        if (!output)
            throw std::runtime_error("Could not write plugin install state");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "Could not replace plugin install state");
#else
        fs::rename(temporary, destination);
#endif
    } catch (...) {
        std::error_code ec;
        fs::remove(temporary, ec);
        throw;
    }
}

} // namespace

LocalPluginStore::LocalPluginStore(fs::path root, Inspector inspector)
    : m_root(fs::absolute(std::move(root)).lexically_normal()), m_inspector(std::move(inspector))
{
    if (!m_inspector)
        throw std::invalid_argument("Plugin metadata inspector is required");
}

std::vector<PluginDescriptor> LocalPluginStore::scan(std::string& error) const
{
    std::vector<PluginDescriptor> result;
    error.clear();
    try {
        if (!fs::exists(m_root))
            return result;
        for (const auto& directory : fs::directory_iterator(m_root)) {
            if (ignored(directory.path()))
                continue;
            if (fs::is_symlink(directory.symlink_status())) {
                PluginDescriptor descriptor;
                descriptor.plugin_key  = make_local_plugin_key(directory.path().stem().u8string());
                descriptor.plugin_root = directory.path().u8string();
                descriptor.error       = "Symlinked plugin directories are not supported";
                result.push_back(std::move(descriptor));
                continue;
            }
            if (!directory.is_directory())
                continue;
            PluginDescriptor descriptor;
            descriptor.plugin_key  = make_local_plugin_key(directory.path().stem().u8string());
            descriptor.plugin_root = directory.path().u8string();
            try {
                std::vector<fs::path> entries;
                for (const auto& file : fs::directory_iterator(directory.path())) {
                    if (ignored(file.path()))
                        continue;
                    const auto extension = file.path().extension();
                    if (extension != ".py" && extension != ".whl")
                        continue;
                    if (fs::is_symlink(file.symlink_status()) || !file.is_regular_file())
                        throw std::runtime_error("Plugin entry must be a regular file inside its directory");
                    entries.push_back(file.path());
                }
                if (entries.size() != 1)
                    throw std::runtime_error("Plugin directory must contain exactly one .py or .whl entry");
                PluginDescriptor parsed;
                std::string      parse_error;
                if (!m_inspector(entries.front(), parsed, parse_error))
                    throw std::runtime_error(parse_error);
                descriptor = std::move(parsed);
                if (!valid_key(descriptor.plugin_key))
                    throw std::runtime_error("Invalid or reserved plugin key");
                descriptor.installed_version   = descriptor.version;
                const auto state               = read_state(directory.path());
                descriptor.enabled             = state.at("enabled").get<bool>();
                descriptor.install_state_valid = true;
            } catch (const std::exception& ex) {
                descriptor.enabled = false;
                descriptor.error   = ex.what();
            }
            result.push_back(std::move(descriptor));
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.plugin_root < b.plugin_root; });
        for (auto& entry : result) {
            if (std::count_if(result.begin(), result.end(), [&](const auto& other) { return other.plugin_key == entry.plugin_key; }) > 1) {
                entry.enabled = false;
                entry.error   = "Duplicate plugin key: " + entry.plugin_key;
            }
        }
    } catch (const std::exception& ex) {
        error = ex.what();
    }
    return result;
}

bool LocalPluginStore::install(const fs::path& source, PluginDescriptor& installed, std::string& error)
{
    error.clear();
    installed = {};
    fs::path staging;
    try {
        fs::create_directories(m_root);
        StoreLock        lock(m_root);
        const auto       source_path = fs::canonical(source);
        PluginDescriptor descriptor;
        if (!m_inspector(source_path, descriptor, error))
            throw std::runtime_error(error);
        if (!valid_key(descriptor.plugin_key))
            throw std::runtime_error("Invalid or reserved plugin key");
        if (!descriptor.dependencies.empty())
            throw std::runtime_error("Dependency installation is not available in this development step");
        std::string scan_error;
        for (const auto& existing : scan(scan_error))
            if (existing.plugin_key == descriptor.plugin_key)
                throw std::runtime_error("Plugin key is already installed; replacement requires the later recovery workflow");
        if (!scan_error.empty())
            throw std::runtime_error(scan_error);
        const auto destination = m_root / filesystem_safe_escape(source_path.filename().u8string());
        if (fs::exists(fs::symlink_status(destination)))
            throw std::runtime_error("Plugin installation directory already exists");
        staging = m_root / (".staging-" + suffix());
        if (!fs::create_directory(staging))
            throw std::runtime_error("Could not create plugin staging directory");
        const auto staged_file = staging / source_path.filename();
        fs::copy_file(source_path, staged_file);
        // Validate staged metadata before publishing the directory.
        PluginDescriptor copied;
        if (!m_inspector(staged_file, copied, error) || copied.plugin_key != descriptor.plugin_key || !copied.dependencies.empty())
            throw std::runtime_error(error.empty() ? "Plugin changed during installation" : error);
        json permissions = json::object();
        for (const auto* category : {"fs_read", "fs_readwrite", "network_http", "network_socket", "process", "threading", "events"})
            permissions[category] = json::array();
        write_state(staging, {{"installed_from", "local"},
                              {"installed_version", copied.version},
                              {"plugin_name", copied.name},
                              {"enabled", true},
                              {"capabilities", json::array()},
                              {"permissions", permissions}});
        fs::rename(staging, destination);
        staging.clear();
        copied.entry_path          = (destination / source_path.filename()).u8string();
        copied.plugin_root         = destination.u8string();
        copied.installed_version   = copied.version;
        copied.enabled             = true;
        copied.install_state_valid = true;
        installed                  = std::move(copied);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        if (!staging.empty()) {
            std::error_code ec;
            fs::remove_all(staging, ec);
        }
        return false;
    }
}

fs::path LocalPluginStore::resolve_package(const std::string& key) const
{
    if (!valid_key(key))
        throw std::runtime_error("Invalid plugin key");
    std::string error;
    auto        entries = scan(error);
    if (!error.empty())
        throw std::runtime_error(error);
    fs::path found;
    for (const auto& entry : entries) {
        if (entry.plugin_key != key)
            continue;
        if (!found.empty())
            throw std::runtime_error("Ambiguous plugin key");
        found = fs::u8path(entry.plugin_root);
    }
    if (found.empty())
        throw std::runtime_error("Plugin is not installed");
    if (fs::is_symlink(fs::symlink_status(found)) || fs::canonical(found).parent_path() != fs::canonical(m_root))
        throw std::runtime_error("Plugin directory is outside the local store");
    return found;
}

bool LocalPluginStore::set_enabled(const std::string& key, bool enabled, std::string& error)
{
    try {
        StoreLock  lock(m_root);
        const auto directory = resolve_package(key);
        auto       state     = read_state(directory);
        state["enabled"]     = enabled;
        write_state(directory, state);
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool LocalPluginStore::remove(const std::string& key, std::string& error)
{
    try {
        StoreLock  lock(m_root);
        const auto directory = resolve_package(key);
        const auto trash     = m_root / (".removed-" + suffix());
        fs::rename(directory, trash);
        fs::remove_all(trash);
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

} // namespace Slic3r
