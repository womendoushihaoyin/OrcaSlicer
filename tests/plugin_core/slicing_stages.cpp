#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/SlicingPipeline.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include <pybind11/embed.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Slic3r;
namespace {
using PipelineStep = SlicingPipelineStepPlugin;
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
struct Detach {
    ~Detach() { set_slicing_pipeline_hook_fn(nullptr); }
};
struct Fixture {
    Model model;
    DynamicPrintConfig config;
    Print print;
    Fixture(bool duplicate = false)
    {
        auto* object = model.add_object();
        object->add_volume(TriangleMesh(make_cube(10., 20., 1.)));
        object->add_instance();
        object->ensure_on_bed();
        if (duplicate) {
            // Model::add_object(other) inserts extruder=0 into the clone. Match
            // that explicit default on the source so they are truly equivalent
            // under Print's sharing predicate (absent != explicit zero).
            object->config.set_key_value("extruder", new ConfigOptionInt(0));
            auto* copy = model.add_object(*object);
            copy->instances.front()->set_offset(Vec3d(30., 0., 0.));
        }
        const auto& defaults = FullPrintConfig::defaults();
        config.apply(static_cast<const PrintObjectConfig&>(defaults), true);
        config.apply(static_cast<const PrintRegionConfig&>(defaults), true);
        config.apply(static_cast<const PrintConfig&>(defaults), true);
        print.apply(model, config);
        print.set_status_silent();
    }
};
void shrink_with_python(const PrintObject* object)
{
    PythonGILState gil;
    require(bool(gil), "Python unavailable during native stage");
    pybind11::module_::import("orca");
    pybind11::dict scope;
    scope["object"] = pybind11::cast(const_cast<PrintObject*>(object), pybind11::return_value_policy::reference);
    pybind11::exec(R"PY(
for layer in object.layers():
    for region in layer.regions():
        for surface in region.slices.surfaces:
            for point in surface.expolygon.contour.points:
                point.x = round(point.x / 2)
    layer.make_slices()
)PY", scope);
}
void check_geometry(const Print& print)
{
    for (const auto* object : print.objects())
        for (const auto* layer : object->layers()) {
            require(!layer->lslices_bboxes.empty(), "missing slice boxes");
            for (const auto& bounds : layer->lslices_bboxes)
                require(bounds.size().x() > scale_(4.9) && bounds.size().x() < scale_(5.1),
                        "incremental slice lost or repeated Python contour modification");
            for (const auto* region : layer->regions())
                for (const auto& polygon : region->raw_slices) {
                    const auto bounds = polygon.contour.bounding_box();
                    require(bounds.size().x() > scale_(4.9) && bounds.size().x() < scale_(5.1),
                            "raw_slices did not retain modified geometry");
                }
        }
}
const std::vector<PipelineStep> expected{
    PipelineStep::posSlice, PipelineStep::posPerimeters, PipelineStep::posEstimateCurledExtrusions,
    PipelineStep::posPrepareInfill, PipelineStep::posInfill, PipelineStep::posIroning,
    PipelineStep::posSupportMaterial, PipelineStep::posDetectOverhangsForLift,
    PipelineStep::psWipeTower, PipelineStep::psSkirtBrim, PipelineStep::posSimplifyPath
};
std::string extrusion_snapshot(const Print& print)
{
    std::ostringstream output;
    output << std::setprecision(17);
    std::function<void(const ExtrusionEntity*)> append = [&](const ExtrusionEntity* entity) {
        if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(entity)) {
            output << "collection " << collection->entities.size() << '\n';
            for (const auto* child : collection->entities) append(child);
        } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
            output << "loop " << loop->paths.size() << '\n';
            for (const auto& path : loop->paths) append(&path);
        } else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
            output << "multi " << multi->paths.size() << '\n';
            for (const auto& path : multi->paths) append(&path);
        } else if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity)) {
            output << "path " << int(path->role()) << ' ' << path->width << ' ' << path->height << ' ' << path->mm3_per_mm << '\n';
            for (const auto& point : path->polyline.points) output << point.x() << ',' << point.y() << ';';
            output << '\n';
        } else throw std::runtime_error("unknown extrusion in baseline fixture");
    };
    for (const auto* object : print.objects())
        for (const auto* layer : object->layers()) {
            output << "layer " << layer->print_z << ' ' << layer->height << '\n';
            for (const auto* region : layer->regions()) {
                append(&region->perimeters);
                append(&region->fills);
            }
        }
    return output.str();
}
} // namespace

void test_slicing_stages(const std::string& scenario)
{
    Detach cleanup;
    if (scenario == "slicing_noop") {
        Fixture baseline;
        baseline.print.process();
        const auto before = extrusion_snapshot(baseline.print);
        require(before.find("path ") != std::string::npos, "empty baseline extrusion graph");
        size_t calls = 0;
        set_slicing_pipeline_hook_fn([&](Print&, const PrintObject*, PipelineStep) { ++calls; });
        Fixture hooked;
        hooked.print.process();
        require(calls == expected.size(), "no-op dispatcher did not traverse real seams");
        require(before == extrusion_snapshot(hooked.print), "no-op hooks changed native perimeter/infill output");
        return;
    }
    if (scenario == "slicing_drain") {
        Fixture fixture;
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        std::atomic<size_t> calls{0};
        set_slicing_pipeline_hook_fn([&](Print&, const PrintObject*, PipelineStep step) {
            ++calls;
            if (step == PipelineStep::posSlice) { entered.set_value(); gate.wait(); }
        });
        auto worker = std::async(std::launch::async, [&] { fixture.print.process(); });
        if (entered.get_future().wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
            release.set_value();
            worker.get();
            throw std::runtime_error("slice worker never entered hook");
        }
        auto drain = std::async(std::launch::async, [] { set_slicing_pipeline_hook_fn(nullptr); });
        const bool blocked = drain.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
        release.set_value();
        worker.get();
        drain.get();
        require(blocked, "detach did not wait for in-progress process pass");
        require(calls == expected.size(), "detach changed dispatcher halfway through a slice");
        return;
    }

    Fixture fixture(scenario == "slicing_shared");
    std::vector<PipelineStep> steps;
    const auto worker_thread = std::this_thread::get_id();
    bool inject_failure = scenario == "slicing_failure";
    bool inject_cancel = scenario == "slicing_cancel";
    set_slicing_pipeline_hook_fn([&](Print& print, const PrintObject* object, PipelineStep step) {
        require(std::this_thread::get_id() == worker_thread, "hook escaped the serial slicing worker");
        require((object == nullptr) == (step == PipelineStep::psWipeTower || step == PipelineStep::psSkirtBrim), "wrong stage scope");
        steps.push_back(step);
        if (step == PipelineStep::posSlice) shrink_with_python(object);
        if (step == PipelineStep::psSkirtBrim) {
            if (inject_failure) throw std::runtime_error("injected pipeline failure");
            if (inject_cancel) print.cancel();
        }
    });
    if (inject_failure || inject_cancel) {
        bool failed = false;
        fixture.print.set_cancel_callback([] { throw std::runtime_error("worker called UI cancellation callback"); });
        try { fixture.print.process(); }
        catch (const CanceledException&) { require(inject_cancel, "unexpected cancellation"); failed = true; }
        catch (const std::runtime_error& ex) {
            require(inject_failure && std::string(ex.what()) == "injected pipeline failure", "wrong pipeline failure");
            failed = true;
        }
        require(failed, "pipeline failure was swallowed");
        require(!fixture.print.is_step_done(posSlice) && !fixture.print.is_step_done(psWipeTower) &&
                    !fixture.print.is_step_done(psSkirtBrim), "failed graph remained reusable");
        fixture.print.set_cancel_callback([] {});
        inject_failure = inject_cancel = false;
        fixture.print.restart();
        steps.clear();
    }
    fixture.print.process();
    if (scenario == "slicing_shared")
        require(std::count_if(fixture.print.objects().begin(), fixture.print.objects().end(),
                             [](const PrintObject* object) { return object->get_shared_object() != nullptr; }) == 1,
                "fixture failed to enter native object sharing");
    if (steps != expected) {
        std::ostringstream message;
        message << "unexpected reachable stages:";
        for (auto step : steps) message << ' ' << int(step);
        message << "; objects=" << fixture.print.objects().size() << "; shared=";
        for (const auto* object : fixture.print.objects()) message << bool(object->get_shared_object()) << ',';
        throw std::runtime_error(message.str());
    }
    require(fixture.print.get_object(size_t(0))->is_step_done(posEstimateCurledExtrusions),
            "curled extrusion stage did not finish before its callback");
    check_geometry(fixture.print);
    steps.clear();
    fixture.print.process();
    require(steps.empty(), "warm slice repeated a cached hook");
    check_geometry(fixture.print);
    // The release on-disk cache cannot preserve plugin identity/config or XYZ
    // paths. A requested disk-cache pass deliberately rebuilds the graph;
    // ordinary warm in-memory passes above must still remain cached.
    fixture.print.process(nullptr, true);
    require(steps == expected, "disk-cache fallback did not rebuild every plugin stage");
    check_geometry(fixture.print);
    steps.clear();

    if (scenario != "slicing_shared") {
        fixture.config.set_key_value("wall_loops", new ConfigOptionInt(4));
        fixture.print.apply(fixture.model, fixture.config);
        fixture.print.process();
        require(std::find(steps.begin(), steps.end(), PipelineStep::posSlice) == steps.end(), "wall edit unnecessarily re-sliced");
        require(std::count(steps.begin(), steps.end(), PipelineStep::posPerimeters) == 1, "wall edit did not regenerate perimeters");
        check_geometry(fixture.print);
        steps.clear();
        fixture.config.set_key_value("sparse_infill_density", new ConfigOptionPercent(12));
        fixture.print.apply(fixture.model, fixture.config);
        fixture.print.process();
        require(std::find(steps.begin(), steps.end(), PipelineStep::posSlice) == steps.end(), "infill edit unnecessarily re-sliced");
        require(std::count(steps.begin(), steps.end(), PipelineStep::posInfill) == 1, "infill edit did not regenerate fills");
        check_geometry(fixture.print);
    }
}
