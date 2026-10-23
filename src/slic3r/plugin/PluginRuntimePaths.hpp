#pragma once

// Private deployment policy, shared by the interpreter and the standalone
// filesystem regression. No application state or current-directory lookup.
#include <filesystem>
#include <vector>

namespace Slic3r::PluginRuntimePaths {

namespace fs = std::filesystem;
enum class Platform { MacOS, Windows, Linux };

inline constexpr Platform native_platform()
{
#ifdef __APPLE__
    return Platform::MacOS;
#elif defined(_WIN32)
    return Platform::Windows;
#else
    return Platform::Linux;
#endif
}

inline bool regular_file(const fs::path& path)
{
    std::error_code error;
    return fs::is_regular_file(path, error);
}

inline std::vector<fs::path> python_candidates(const fs::path& executable, const fs::path& resources,
                                             Platform platform, const fs::path& development)
{
    std::vector<fs::path> result;
    if (executable.is_absolute()) {
        if (platform == Platform::MacOS)
            result.push_back(executable.parent_path().parent_path() / "Frameworks" / "python");
        else if (platform == Platform::Linux)
            result.push_back(executable.parent_path().parent_path() / "lib" / "python");
    }
    // Retain the resource-relative layout for existing multi-config/FHS
    // deployments, but an unset resources_dir must not turn into a CWD probe.
    if (platform == Platform::Linux && resources.is_absolute())
        result.push_back(resources.parent_path() / "lib" / "python");
    if (executable.is_absolute())
        result.push_back(executable.parent_path() / "python");
    if (development.is_absolute())
        result.push_back(development);
    return result;
}

inline bool valid_python_home(const fs::path& root, Platform platform)
{
    if (platform == Platform::Windows)
        return regular_file(root / "Lib" / "encodings" / "__init__.py") &&
               (regular_file(root / "python312.dll") || regular_file(root / "python312_d.dll"));
    return regular_file(root / "lib" / "python3.12" / "encodings" / "__init__.py");
}

inline fs::path python_home(const fs::path& executable, const fs::path& resources,
                            Platform platform, const fs::path& development)
{
    for (const auto& candidate : python_candidates(executable, resources, platform, development))
        if (valid_python_home(candidate, platform))
            return candidate;
    return {};
}

inline std::vector<fs::path> uv_candidates(const fs::path& executable, const fs::path& resources,
                                         Platform platform, const fs::path& development)
{
    const char* filename = platform == Platform::Windows ? "uv.exe" : "uv";
    std::vector<fs::path> result;
    if (resources.is_absolute())
        result.push_back(resources / "tools" / "uv" / filename);
    if (executable.is_absolute()) {
        if (platform == Platform::Linux)
            result.push_back(executable.parent_path().parent_path() / "resources" / "tools" / "uv" / filename);
        result.push_back(executable.parent_path() / "tools" / "uv" / filename);
    }
    if (development.is_absolute())
        result.push_back(development);
    return result;
}

inline fs::path uv(const fs::path& executable, const fs::path& resources,
                   Platform platform, const fs::path& development)
{
    for (const auto& candidate : uv_candidates(executable, resources, platform, development))
        if (regular_file(candidate))
            return candidate;
    return {};
}

} // namespace Slic3r::PluginRuntimePaths
