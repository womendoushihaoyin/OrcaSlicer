#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/GatewayAccount.hpp"
#include "slic3r/plugin/host/PluginAppInfoIdentity.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/log/core.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace fixture_fs = std::filesystem;
using Slic3r::NetworkAgent;

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string library_name(const char* stem)
{
#ifdef _WIN32
    return std::string(stem) + ".dll";
#elif defined(__APPLE__)
    return "lib" + std::string(stem) + ".dylib";
#else
    return "lib" + std::string(stem) + ".so";
#endif
}

struct Fixture
{
    fixture_fs::path root;
    fixture_fs::path libraries;
    fixture_fs::path plugins;
    fixture_fs::path network;

    void begin(const std::string& name)
    {
        NetworkAgent::unload_network_module();
        const auto profile = root / fixture_fs::u8path(name);
        plugins = profile / "plugins";
        fixture_fs::create_directories(plugins);
        network = plugins / library_name(BAMBU_NETWORK_LIBRARY);
        Slic3r::set_data_dir(profile.u8string());
    }

    void copy_network(const char* variant)
    {
        fixture_fs::copy_file(libraries / library_name(variant), network, fixture_fs::copy_options::overwrite_existing);
    }
};

void check_identity_mapping()
{
    const std::set<std::string> expected{
        "version", "build", "mode", "language", "app_config_version", "stealth_mode", "is_signed_in",
        "is_bambu_signed_in", "network_plugin_version", "network_plugin_error"};
    for (bool snapmaker : {false, true}) {
        Slic3r::Gateway::AccountSnapshot account;
        require(Slic3r::Gateway::GatewayAccount::parse({{"is_login", snapmaker}, {"userid", "fixture-user"},
                                                       {"token", "fixture-only-not-a-secret"}}, account),
                "Actual gateway parser rejected fixture");
        for (bool bambu : {false, true}) {
            nlohmann::json info{{"version", "fixture"}, {"build", "fixture"}, {"mode", "editor"}, {"language", "zh_CN"},
                                {"app_config_version", ""}, {"stealth_mode", false}};
            Slic3r::append_plugin_app_info_identity(info, account.is_login, bambu, {});
            std::set<std::string> actual;
            for (auto it = info.begin(); it != info.end(); ++it)
                actual.insert(it.key());
            require(actual == expected, "Host app-info key set differs from the fixed upstream contract");
            require(info["is_signed_in"].is_boolean() && info["is_signed_in"] == snapmaker, "Snapmaker identity conflated");
            require(info["is_bambu_signed_in"].is_boolean() && info["is_bambu_signed_in"] == bambu, "Bambu identity conflated");
            require(info["network_plugin_version"].is_string() && info["network_plugin_version"] == "", "Absent version fabricated");
            require(info["network_plugin_error"].is_string() && info["network_plugin_error"] == "", "Fresh error fabricated");
            require(info.dump().find("fixture-user") == std::string::npos &&
                        info.dump().find("fixture-only-not-a-secret") == std::string::npos,
                    "Account identity details leaked through app_info");
        }
    }
    std::cout << "PASS identity_mapping_four_combinations (pure mapping, not GUI)\n";
}
} // namespace

int main(int argc, char** argv)
{
    boost::nowide::args utf8_args(argc, argv);
    boost::nowide::nowide_filesystem();
    if (argc != 3)
        return 2;
    boost::log::core::get()->set_logging_enabled(false);
    try {
        Fixture fixture{fixture_fs::u8path(argv[1]), fixture_fs::u8path(argv[2]), {}, {}};
        require(!fixture_fs::exists(fixture.root), "Use a fresh isolated profile root");
        const auto fresh = NetworkAgent::network_module_snapshot();
        require(!fresh.loaded && fresh.version.empty() && fresh.error.empty(), "Fresh host module state incorrect");
        check_identity_mapping();

        fixture.begin("missing");
        require(NetworkAgent::initialize_network_module() != 0, "Missing library unexpectedly loaded");
        const auto missing = NetworkAgent::network_module_snapshot();
        require(!missing.loaded && missing.version.empty() && missing.error.find(BAMBU_NETWORK_LIBRARY) != std::string::npos,
                "Missing library has no real loader diagnostic");
        NetworkAgent::unload_network_module();
        require(NetworkAgent::network_module_snapshot().error == missing.error, "Cleanup erased load failure");
        std::cout << "PASS missing_library_cleanup_retains_error\n";

        fixture.begin("corrupt");
        { std::ofstream stream(fixture.network, std::ios::binary); stream << "invalid native library fixture\n"; }
        require(NetworkAgent::initialize_network_module() != 0, "Corrupt native library unexpectedly loaded");
        const auto corrupt = NetworkAgent::network_module_snapshot();
        require(!corrupt.loaded && corrupt.version.empty() && !corrupt.error.empty() && corrupt.error != missing.error,
                "Corrupt library did not preserve its own loader error");
        std::cout << "PASS corrupt_library_real_loader_error\n";

        fixture.begin("成功恢复-中文");
        fixture.copy_network("app_info_good");
        require(NetworkAgent::initialize_network_module() == 0, "Valid native module failed to load");
        auto good = NetworkAgent::network_module_snapshot();
        require(good.loaded && good.version == "01.02.03.04" && good.error.empty(), "Successful retry did not clear old failure");
        require(NetworkAgent::get_bambu_source_entry() == nullptr, "Missing source unexpectedly loaded");
        const auto no_source = NetworkAgent::network_module_snapshot();
        require(no_source.loaded && no_source.version == good.version && no_source.error.find("BambuSource") != std::string::npos,
                "Source dependency load failure not observable");
        fixture_fs::copy_file(fixture.libraries / library_name("app_info_good"), fixture.plugins / library_name("BambuSource"));
        require(NetworkAgent::get_bambu_source_entry() != nullptr, "Valid source module did not load");
        require(NetworkAgent::network_module_snapshot().error.empty(), "Successful source retry did not clear its failure");
        NetworkAgent::unload_network_module();
        good = NetworkAgent::network_module_snapshot();
        require(!good.loaded && good.version.empty() && good.error.empty(), "Successful unload leaves stale version or false error");
        std::cout << "PASS real_module_unicode_source_retry_and_unload\n";

        fixture.begin("missing-version");
        fixture.copy_network("app_info_no_version");
        require(NetworkAgent::initialize_network_module() == 0, "Optional-version behavior changed");
        const auto no_version = NetworkAgent::network_module_snapshot();
        require(no_version.loaded && no_version.version.empty() && no_version.error.find("bambu_network_get_version") != std::string::npos,
                "Missing version symbol became fabricated version or silent success");
        require(NetworkAgent::get_version() == "00.00.00.00", "Legacy caller return behavior changed");
        NetworkAgent::unload_network_module();
        require(NetworkAgent::network_module_snapshot().error == no_version.error, "Unload erased missing-symbol diagnosis");
        std::cout << "PASS missing_symbol_preserves_legacy_return_without_app_info_sentinel\n";

        fixture.begin("debug-mismatch");
        fixture.copy_network("app_info_debug_mismatch");
        require(NetworkAgent::initialize_network_module() == 0, "Debug mismatch load behavior changed");
        const auto mismatch = NetworkAgent::network_module_snapshot();
        require(mismatch.loaded && mismatch.version.empty() && mismatch.error.find("debug/release") != std::string::npos,
                "Debug mismatch not observable");
        require(NetworkAgent::get_version() == "00.00.00.00", "Legacy debug-mismatch return changed");
        std::cout << "PASS debug_mismatch_observed\n";

        fixture.begin("compatibility");
        fixture.copy_network("app_info_good");
        require(NetworkAgent::initialize_network_module() == 0, "Compatibility fixture load failed");
        NetworkAgent::record_network_module_compatibility_error("99.88.77.66", "01.02.03.04");
        NetworkAgent::unload_network_module();
        const auto incompatible = NetworkAgent::network_module_snapshot();
        require(!incompatible.loaded && incompatible.version.empty() && incompatible.error.find("99.88.77") != std::string::npos &&
                    incompatible.error.find("01.02.03.04") != std::string::npos,
                "Version rejection lost expected/observed diagnostic after cleanup");
        require(NetworkAgent::initialize_network_module() == 0 && NetworkAgent::network_module_snapshot().error.empty(),
                "Successful module reload did not reset compatibility error");
        NetworkAgent::unload_network_module();
        std::cout << "PASS compatibility_diagnostic_cleanup_and_successful_retry\n";

        std::atomic<bool> done{false};
        std::atomic<bool> consistent{true};
        std::thread reader([&] {
            while (!done.load()) {
                const auto snapshot = NetworkAgent::network_module_snapshot();
                if (!snapshot.error.empty() || (snapshot.loaded ? snapshot.version != "01.02.03.04" : !snapshot.version.empty()))
                    consistent.store(false);
            }
        });
        for (int index = 0; index < 20; ++index) {
            if (NetworkAgent::initialize_network_module() != 0)
                consistent.store(false);
            NetworkAgent::unload_network_module();
        }
        done.store(true);
        reader.join();
        require(consistent.load(), "Module snapshots raced actual lifecycle changes");
        std::cout << "PASS concurrent_module_snapshot_lifecycle (not business-call thread safety)\n";
        return 0;
    } catch (const std::exception& error) {
        NetworkAgent::unload_network_module();
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
