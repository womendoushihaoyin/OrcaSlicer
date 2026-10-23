#include "PluginSlicing.hpp"
#include "libslic3r/GCode/PluginPostProcess.hpp"
#include <boost/filesystem.hpp>
#include "PluginConfig.hpp"
#include "PluginManager.hpp"
#include "PythonInterpreter.hpp"
#include "pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp"
#include "libslic3r/Exception.hpp"
#include "common_func/common_func.hpp"
#include <boost/log/trivial.hpp>
#include <set>

namespace Slic3r {
namespace {
struct SliceConsumers {
    std::vector<std::shared_ptr<SlicingPipelinePluginCapability>> capabilities;
    SlicingPluginExecutionScope::Configs configs;
    std::string signature;
};
std::shared_ptr<SliceConsumers> prepare_consumers(const DynamicPrintConfig& config)
{
    const auto* names = config.option<ConfigOptionStrings>("slicing_pipeline_plugin");
    if (!names || names->values.empty()) return {};
    const auto* manifest = config.option<ConfigOptionStrings>("plugins");
    if (!manifest) throw SlicingError("Slicing plugin references are missing from the print preset");
    CapabilityConfigDocument overrides;
    const auto* raw = config.option<ConfigOptionString>("print_plugin_config_overrides");
    std::string error;
    if (!parse_plugin_overrides(raw ? raw->value : std::string(), overrides, error)) throw SlicingError(error);

    auto consumers = std::make_shared<SliceConsumers>();
    auto& manager = PluginManager::instance();
    std::set<std::string> seen;
    auto signature = nlohmann::json::array();
    // Resolve the complete selection before any plugin can change the graph.
    for (const auto& name : names->values) {
        if (name.empty() || !seen.insert(name).second) throw SlicingError("Empty or duplicate slicing plugin selection: " + name);
        std::optional<PluginCapabilityRef> reference;
        for (const auto& item : manifest->values) {
            auto parsed = parse_capability_ref(item);
            if (!parsed) throw SlicingError("Invalid plugin reference in the print preset: " + item);
            if (parsed->capability_name != name) continue;
            if (reference) throw SlicingError("Ambiguous slicing plugin reference: " + name);
            reference = std::move(parsed);
        }
        if (!reference) throw SlicingError("No package reference for slicing plugin: " + name);
        const PluginCapabilityId id{PluginCapabilityType::SlicingPipeline, name,
                                    reference->uuid.empty() ? reference->name : reference->uuid};
        auto cap = std::dynamic_pointer_cast<SlicingPipelinePluginCapability>(manager.acquire_capability(id, error));
        if (!cap) throw SlicingError(error.empty() ? "Capability does not implement the slicing interface: " + name : error);
        consumers->configs.emplace(id, PresetPluginConfigService().get_effective_config(overrides, id));
        signature.push_back({id.plugin_key, id.name, cap->runtime_generation(), consumers->configs.at(id).config});
        consumers->capabilities.push_back(std::move(cap));
    }
    consumers->signature = signature.dump();
    return consumers;
}
SlicingPipelinePlan prepare_slice(Print& print)
{
    auto consumers = prepare_consumers(print.full_print_config());
    if (!consumers) return {};
    SlicingPipelineHookFn dispatch = [consumers](Print& print, const PrintObject* object, SlicingPipelineStepPlugin step) {
        SlicingPluginExecutionScope scope(consumers->configs);
        for (const auto& cap : consumers->capabilities) {
            if (print.canceled()) throw CanceledException();
            ExecutionResult result;
            try {
                PythonGILState gil;
                if (!gil) throw std::runtime_error("Python interpreter is shutting down");
                SlicingPipelineContext context;
                context.orca_version = Snapmaker_VERSION;
                context.step = step;
                context.print = &print;
                context.object = object;
                result = cap->execute(context);
            } catch (const CanceledException&) { throw; }
            catch (const std::exception& ex) {
                throw SlicingError("Slicing plugin '" + cap->audit_plugin_key() + "/" + cap->name() + "': " + ex.what());
            }
            // Match the fixed upstream contract: RecoverableError is logged and
            // processing continues; FatalError or a Python exception stops slicing.
            if (result.status == PluginResult::FatalError)
                throw SlicingError("Slicing plugin '" + cap->name() + "': " + result.message);
            if (!result.message.empty())
                BOOST_LOG_TRIVIAL(info) << "Slicing plugin '" << cap->name() << "' [" << int(step) << "]: " << result.message;
            if (print.canceled()) throw CanceledException();
        }
    };
    return {std::move(dispatch), consumers->signature};
}
GCodePluginHookFn prepare_gcode(const DynamicPrintConfig& config)
{
    auto consumers = prepare_consumers(config);
    if (!consumers) return {};
    // Keep an owned snapshot: scripts and UI may otherwise alter the selected preset.
    return [consumers, config](const std::string& path, const std::string& host, const std::string& name) {
        SlicingPluginExecutionScope scope(consumers->configs);
        for (const auto& cap : consumers->capabilities) {
            ExecutionResult result;
            try {
                PythonGILState gil;
                if (!gil) throw std::runtime_error("Python interpreter is shutting down");
                SlicingPipelineContext context;
                context.orca_version = Snapmaker_VERSION;
                context.step = SlicingPipelineStepPlugin::psGCodePostProcess;
                context.gcode_path = path;
                context.host = host;
                context.output_name = name;
                context.full_config = &config;
                result = cap->execute(context);
            } catch (const std::exception& ex) {
                throw RuntimeError("Post-processing plugin '" + cap->audit_plugin_key() + "/" + cap->name() + "': " + ex.what());
            }
            // At this seam BOTH failure statuses abort export (the pinned upstream contract).
            if (result.status == PluginResult::RecoverableError || result.status == PluginResult::FatalError)
                throw RuntimeError("Post-processing plugin '" + cap->name() + "': " + result.message);
            if (!boost::filesystem::is_regular_file(path))
                throw RuntimeError("Post-processing plugin '" + cap->name() + "' deleted the G-code working file");
            if (!result.message.empty()) BOOST_LOG_TRIVIAL(info) << "Post-processing plugin '" << cap->name() << "': " << result.message;
        }
    };
}
}
void install_slicing_plugin_dispatcher()
{
    set_slicing_pipeline_hook_factory(prepare_slice);
    set_gcode_plugin_hook_factory(prepare_gcode);
}
}
