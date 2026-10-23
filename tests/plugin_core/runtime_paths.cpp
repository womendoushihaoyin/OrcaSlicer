#include "slic3r/plugin/PluginRuntimePaths.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace paths = Slic3r::PluginRuntimePaths;
namespace fs = std::filesystem;

static void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

static void file(const fs::path& path)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "fixture";
}

static void python(const fs::path& root, paths::Platform platform)
{
    file(root / (platform == paths::Platform::Windows ? "Lib" : "lib/python3.12") / "encodings/__init__.py");
    if (platform == paths::Platform::Windows)
        file(root / "python312.dll");
}

static int test_paths(const fs::path& input)
{
    const auto output = fs::absolute(input);
    if (fs::exists(output))
        return 3;
    try {
        fs::create_directories(output);
        const auto previous = fs::current_path();
        const auto cwd = output / "unrelated cwd";
        python(cwd / "lib/python", paths::Platform::Linux);
        file(cwd / "tools/uv/uv");
        fs::current_path(cwd);
        require(paths::python_home({}, {}, paths::Platform::Linux, {}).empty(), "Empty paths searched CWD Python");
        require(paths::uv({}, {}, paths::Platform::Linux, {}).empty(), "Empty paths searched CWD uv");
        require(paths::python_home("bin/host", "resources", paths::Platform::Linux, "lib/python").empty(),
                "Relative arguments searched CWD Python");
        require(paths::uv("bin/host", "resources", paths::Platform::Linux, "tools/uv/uv").empty(),
                "Relative arguments searched CWD uv");
        fs::current_path(previous);
        std::cout << "PASS no CWD fallback\n";

        for (const auto platform : {paths::Platform::MacOS, paths::Platform::Windows, paths::Platform::Linux}) {
            const auto name = platform == paths::Platform::MacOS ? "macos" : platform == paths::Platform::Windows ? "windows" : "linux";
            const auto root = output / name;
            const fs::path host = platform == paths::Platform::MacOS ? "Contents/MacOS/host" :
                                  platform == paths::Platform::Windows ? "host.exe" : "bin/host";
            const fs::path home = platform == paths::Platform::MacOS ? "Contents/Frameworks/python" :
                                  platform == paths::Platform::Windows ? "python" : "lib/python";
            const fs::path uv = platform == paths::Platform::MacOS ? "Contents/MacOS/tools/uv/uv" :
                                platform == paths::Platform::Windows ? "tools/uv/uv.exe" : "resources/tools/uv/uv";
            const auto development = output / (std::string("development-") + name);
            python(development, platform);
            file(development / "uv");
            python(root / home, platform);
            file(root / uv);
            require(paths::python_home(root / host, {}, platform, development) == root / home, "Bundled Python must win");
            require(paths::uv(root / host, {}, platform, development / "uv") == root / uv, "Bundled uv must win");
            const auto moved = output / fs::u8path(std::string("relocated 空格 ") + name);
            fs::rename(root, moved);
            require(paths::python_home(moved / host, {}, platform, development) == moved / home, "Python relocation failed");
            require(paths::uv(moved / host, {}, platform, development / "uv") == moved / uv, "uv relocation failed");
            if (platform == paths::Platform::Windows) {
                fs::remove(moved / home / "python312.dll");
                require(paths::python_home(moved / host, {}, platform, {}).empty(), "Missing Windows DLL accepted");
            }
            fs::remove_all(moved / home);
            fs::remove(moved / uv);
            require(paths::python_home(moved / host, {}, platform, development) == development, "Explicit development fallback failed");
            require(paths::uv(moved / host, {}, platform, development / "uv") == development / "uv", "Explicit uv fallback failed");
            std::cout << "PASS " << name << " layout, relocation and fallback\n";
        }
        const auto fhs = output / "fhs";
        python(fhs / "lib/python", paths::Platform::Linux);
        file(fhs / "share/Snapmaker_Orca/tools/uv/uv");
        require(paths::python_home(fhs / "bin/host", fhs / "share/Snapmaker_Orca", paths::Platform::Linux, {}) == fhs / "lib/python",
                "FHS executable prefix was lost");
        require(paths::uv(fhs / "bin/host", fhs / "share/Snapmaker_Orca", paths::Platform::Linux, {}) ==
                fhs / "share/Snapmaker_Orca/tools/uv/uv", "FHS resources uv was lost");
        std::cout << "PASS FHS prefix and resources\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    return argc == 2 ? test_paths(fs::path(argv[1])) : 2;
}
#else
int main(int argc, char** argv)
{
    return argc == 2 ? test_paths(fs::u8path(argv[1])) : 2;
}
#endif
