#include "PostProcessor.hpp"
#include "PluginPostProcess.hpp"
#include <boost/nowide/cstdio.hpp>
#include <cerrno>
#include <system_error>

#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/I18N.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

// BBS
#include <iostream>
#include <fstream>

#ifdef WIN32

// The standard Windows includes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

// https://blogs.msdn.microsoft.com/twistylittlepassagesallalike/2011/04/23/everyone-quotes-command-line-arguments-the-wrong-way/
// This routine appends the given argument to a command line such that CommandLineToArgvW will return the argument string unchanged.
// Arguments in a command line should be separated by spaces; this function does not add these spaces.
// Argument    - Supplies the argument to encode.
// CommandLine - Supplies the command line to which we append the encoded argument string.
static void quote_argv_winapi(const std::wstring &argument, std::wstring &commmand_line_out)
{
	// Don't quote unless we actually need to do so --- hopefully avoid problems if programs won't parse quotes properly.
	if (argument.empty() == false && argument.find_first_of(L" \t\n\v\"") == argument.npos)
		commmand_line_out.append(argument);
	else {
		commmand_line_out.push_back(L'"');
		for (auto it = argument.begin(); ; ++ it) {
			unsigned number_backslashes = 0;
			while (it != argument.end() && *it == L'\\') {
				++ it;
				++ number_backslashes;
			}
			if (it == argument.end()) {
				// Escape all backslashes, but let the terminating double quotation mark we add below be interpreted as a metacharacter.
				commmand_line_out.append(number_backslashes * 2, L'\\');
				break;
			} else if (*it == L'"') {
				// Escape all backslashes and the following double quotation mark.
				commmand_line_out.append(number_backslashes * 2 + 1, L'\\');
				commmand_line_out.push_back(*it);
			} else {
				// Backslashes aren't special here.
				commmand_line_out.append(number_backslashes, L'\\');
				commmand_line_out.push_back(*it);
			}
		}
		commmand_line_out.push_back(L'"');
	}
}

static DWORD execute_process_winapi(const std::wstring &command_line)
{
    // Extract the current environment to be passed to the child process.
	std::wstring envstr;
	{
		wchar_t *env = GetEnvironmentStrings();
		assert(env != nullptr);
		const wchar_t* var = env;
		size_t totallen = 0;
		size_t len;
		while ((len = wcslen(var)) > 0) {
			totallen += len + 1;
			var += len + 1;
		}
		envstr = std::wstring(env, totallen);
		FreeEnvironmentStrings(env);
	}

	STARTUPINFOW startup_info;
	memset(&startup_info, 0, sizeof(startup_info));
	startup_info.cb			 = sizeof(STARTUPINFO);
#if 0
	startup_info.dwFlags	 = STARTF_USESHOWWINDOW;
	startup_info.wShowWindow = SW_HIDE;
#endif
	PROCESS_INFORMATION process_info;
	if (! ::CreateProcessW(
            nullptr /* lpApplicationName */, (LPWSTR)command_line.c_str(), nullptr /* lpProcessAttributes */, nullptr /* lpThreadAttributes */, false /* bInheritHandles */,
			CREATE_UNICODE_ENVIRONMENT /* | CREATE_NEW_CONSOLE */ /* dwCreationFlags */, (LPVOID)envstr.c_str(), nullptr /* lpCurrentDirectory */, &startup_info, &process_info))
		throw Slic3r::RuntimeError(std::string("Failed starting the script ") + boost::nowide::narrow(command_line) + ", Win32 error: " + std::to_string(int(::GetLastError())));
	::WaitForSingleObject(process_info.hProcess, INFINITE);
	ULONG rc = 0;
	::GetExitCodeProcess(process_info.hProcess, &rc);
	::CloseHandle(process_info.hThread);
	::CloseHandle(process_info.hProcess);
	return rc;
}

// Run the script. If it is a perl script, run it through the bundled perl interpreter.
// If it is a batch file, run it through the cmd.exe.
// Otherwise run it directly.
static int run_script(const std::string &script, const std::string &gcode, std::string &/*std_err*/)
{
    // Unpack the argument list provided by the user.
    int     nArgs;
    LPWSTR *szArglist = CommandLineToArgvW(boost::nowide::widen(script).c_str(), &nArgs);
    if (szArglist == nullptr || nArgs <= 0) {
        // CommandLineToArgvW failed. Maybe the command line escapment is invalid?
		throw Slic3r::RuntimeError(std::string("Post processing script ") + script + " on file " + gcode + " failed. CommandLineToArgvW() refused to parse the command line path.");
    }

    std::wstring command_line;
    std::wstring command = szArglist[0];
	if (! boost::filesystem::exists(boost::filesystem::path(command)))
		throw Slic3r::RuntimeError(std::string("The configured post-processing script does not exist: ") + boost::nowide::narrow(command));
    if (boost::iends_with(command, L".pl")) {
        // This is a perl script. Run it through the perl interpreter.
        // The current process may be slic3r.exe or slic3r-console.exe.
        // Find the path of the process:
        wchar_t wpath_exe[_MAX_PATH + 1];
        ::GetModuleFileNameW(nullptr, wpath_exe, _MAX_PATH);
        boost::filesystem::path path_exe(wpath_exe);
        boost::filesystem::path path_perl = path_exe.parent_path() / "perl" / "perl.exe";
        if (! boost::filesystem::exists(path_perl)) {
			LocalFree(szArglist);
			throw Slic3r::RuntimeError(std::string("Perl interpreter ") + path_perl.string() + " does not exist.");
        }
        // Replace it with the current perl interpreter.
        quote_argv_winapi(boost::nowide::widen(path_perl.string()), command_line);
        command_line += L" ";
    } else if (boost::iends_with(command, ".bat")) {
        // Run a batch file through the command line interpreter.
        command_line = L"cmd.exe /C ";
    }

    for (int i = 0; i < nArgs; ++ i) {
        quote_argv_winapi(szArglist[i], command_line);
        command_line += L" ";
    }
    LocalFree(szArglist);
	quote_argv_winapi(boost::nowide::widen(gcode), command_line);
    return (int)execute_process_winapi(command_line);
}

#else
    // POSIX

#include <cstdlib>   // getenv()
#include <sstream>
#include <boost/process.hpp>

namespace process = boost::process;

static int run_script(const std::string &script, const std::string &gcode, std::string &std_err)
{
    // Try to obtain user's default shell
    const char *shell = ::getenv("SHELL");
    if (shell == nullptr) { shell = "/bin/sh"; }

    // Quote and escape the gcode path argument
    std::string command { script };
    command.append(" '");
    for (char c : gcode) {
        if (c == '\'') { command.append("'\\''"); }
        else { command.push_back(c); }
    }
    command.push_back('\'');

    BOOST_LOG_TRIVIAL(debug) << boost::format("Executing script, shell: %1%, command: %2%") % shell % command;

    process::ipstream istd_err;
    process::child child(shell, "-c", command, process::std_err > istd_err);

    std_err.clear();
    std::string line;

    while (child.running() && std::getline(istd_err, line)) {
        std_err.append(line);
        std_err.push_back('\n');
    }

    child.wait();
    return child.exit_code();
}

#endif

namespace Slic3r {

//! macro used to mark string used at localization,
//! return same string
#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

// BBS
void gcode_add_line_number(const std::string& path, const DynamicPrintConfig& config)
{
    const ConfigOptionBool* opt = config.opt<ConfigOptionBool>("gcode_add_line_number");
    if (!opt->getBool())
        return;

    auto gcode_file = boost::filesystem::path(path);
    if (!boost::filesystem::exists(gcode_file))
        return;

    std::fstream fs;
    std::string new_gcode;
    fs.open(gcode_file.c_str(), std::fstream::in | std::fstream::out);

    size_t line_number = 1;
    std::string gcode_line;
    while (std::getline(fs, gcode_line)) {
        char num_str[128];
        memset(num_str, 0, sizeof(num_str));
        snprintf(num_str, sizeof(num_str), "%zd", line_number);
        new_gcode += std::string("N") + num_str + " " + gcode_line + "\n";
        line_number++;
    }

    fs.clear();
    fs.seekp(0, std::ios_base::beg);
    fs.write(new_gcode.c_str(), new_gcode.length());
    fs.close();
}

// Classic scripts run before the selected slicing plugins' final G-code seam.
// Plugin jobs always use a working copy. On success, make_copy=false commits it
// over the input without changing its path (also required by the upload caller).
bool run_post_process_scripts(std::string &src_path, bool make_copy, const std::string &host, std::string &output_name, const DynamicPrintConfig &config)
{
    namespace fs = boost::filesystem;
    const auto* scripts = config.opt<ConfigOptionStrings>("post_process");
    const auto* plugins = config.opt<ConfigOptionStrings>("slicing_pipeline_plugin");
    const bool have_scripts = scripts && !scripts->values.empty();
    const bool have_plugins = plugins && !plugins->values.empty();
    if (!have_scripts && !have_plugins) return false;

    // Preflight the complete selection and hold its runtime leases through scripts,
    // callbacks and commit. A missing runtime never silently exports different data.
    GCodePluginLease dispatch(config);
    if (have_plugins && !dispatch) throw RuntimeError("G-code plugin runtime is unavailable");
    const std::string original = src_path;
    std::string candidate_name = output_name;
    const bool working_copy = make_copy || have_plugins;
    const std::string path = have_plugins ? fs::unique_path(original + ".%%%%-%%%%-%%%%.pp").string() :
                             make_copy ? original + ".pp" : original;
    const std::string name_file = path + ".output_name";
    auto remove_file = [](const std::string& file) {
        boost::system::error_code ec;
        fs::remove(file, ec);
        if (ec) BOOST_LOG_TRIVIAL(warning) << "Post-processing temporary file cleanup failed: " << file << ": " << ec.message();
    };
    auto read_output_name = [&] {
        if (!fs::exists(name_file)) return;
        boost::nowide::ifstream input(name_file);
        std::string name;
        if (!std::getline(input, name) || name.empty()) throw RuntimeError("Unable to read desired new path from output name file");
        input.close();
        if (host == "File") {
            fs::path target(name);
            if (target.is_relative() && target.has_filename() && target.parent_path().empty())
                target = fs::path(candidate_name).parent_path() / target;
            if (!target.is_absolute() || !target.has_filename()) throw RuntimeError("Unable to parse desired new path from output name file");
            if (!fs::is_directory(target.parent_path())) throw RuntimeError("Output directory does not exist: " + target.parent_path().string());
            name = target.string();
        }
        candidate_name = std::move(name);
        remove_file(name_file);
    };
    try {
        if (!fs::is_regular_file(original)) throw RuntimeError("Post-processor can't find exported gcode file");
        if (working_copy) {
            remove_file(path);
            std::string error;
            if (copy_file(original, path, error, false) != SUCCESS)
                throw RuntimeError("Failed making a temporary copy of G-code file: " + error);
        }
        remove_file(name_file);
        config.setenv_();
        boost::nowide::setenv("SLIC3R_PP_HOST", host.c_str(), 1);
        boost::nowide::setenv("SLIC3R_PP_OUTPUT_NAME", candidate_name.c_str(), 1);
        if (have_scripts) for (const auto& script_list : scripts->values) {
            std::vector<std::string> lines;
            boost::split(lines, script_list, boost::is_any_of("\r\n"));
            for (auto script : lines) {
                boost::trim(script);
                if (script.empty()) continue;
                std::string stderr_text;
                const int result = run_script(script, path, stderr_text);
                if (result != 0)
                    throw RuntimeError(Slic3r::format("Post-processing script %1% failed. Error code: %2%\n%3%", script, result, stderr_text));
                if (!fs::is_regular_file(path)) throw RuntimeError("Post-processing script deleted the G-code working file");
            }
        }
        read_output_name();
        if (have_plugins) {
            boost::nowide::setenv("SLIC3R_PP_OUTPUT_NAME", candidate_name.c_str(), 1);
            dispatch(path, host, candidate_name);
        }
        // The fixed upstream plugin contract exposes a read-only output_name;
        // only classic scripts rename it. Do not leave an unsupported sidecar behind.
        remove_file(name_file);
        if (have_plugins && !make_copy) {
            // Do not use rename_file(): its POSIX implementation deletes the old
            // destination first, losing the original if the subsequent rename fails.
#ifdef _WIN32
            if (!MoveFileExW(boost::nowide::widen(path).c_str(), boost::nowide::widen(original).c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw RuntimeError("Failed committing post-processed G-code: " + std::system_category().message(GetLastError()));
#else
            if (boost::nowide::rename(path.c_str(), original.c_str()) != 0)
                throw RuntimeError("Failed committing post-processed G-code: " + std::generic_category().message(errno));
#endif
        }
    } catch (...) {
        remove_file(name_file);
        if (working_copy) remove_file(path);
        throw;
    }
    // Delay caller-visible changes until success. src_path and output_name may
    // alias in BackgroundSlicingProcess's BBL path; its source path must win.
    output_name = std::move(candidate_name);
    src_path = make_copy ? path : original;
    return true;
}

} // namespace Slic3r
