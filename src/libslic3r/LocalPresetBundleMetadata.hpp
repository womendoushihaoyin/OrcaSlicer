#pragma once

#include <string>
#include <vector>

namespace Slic3r {

// Runtime provenance for local bundles only. Cloud subscription state is not
// fabricated by the release_2.4.1 local-plugin adapter.
struct LocalPresetBundleMetadata
{
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::string path;
    long long imported_time = 0;
    long long updated_time = 0;
    std::vector<std::string> print_presets;
    std::vector<std::string> filament_presets;
    std::vector<std::string> printer_presets;
};

} // namespace Slic3r
