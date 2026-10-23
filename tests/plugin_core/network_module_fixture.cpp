#include <string>

#ifdef _WIN32
#define FIXTURE_EXPORT extern "C" __declspec(dllexport)
#else
#define FIXTURE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// This library performs no connection, account, filesystem or device operation.
// It is a real native module for exercising the production dynamic loader.
FIXTURE_EXPORT int app_info_fixture_anchor() { return 1; }

#ifndef APP_INFO_FIXTURE_NO_VERSION
FIXTURE_EXPORT std::string bambu_network_get_version() { return "01.02.03.04"; }
#endif

FIXTURE_EXPORT bool bambu_network_check_debug_consistent(bool)
{
#ifdef APP_INFO_FIXTURE_DEBUG_MISMATCH
    return false;
#else
    return true;
#endif
}
