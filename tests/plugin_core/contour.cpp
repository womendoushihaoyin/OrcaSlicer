#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/Path3Projection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SlicingPipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace Slic3r;
namespace test_fs = std::filesystem;

namespace Slic3r {
// Keep the consumer regression on the real writer and extrusion methods. The
// exporter initializes their state before these focused transitions are tested.
struct GCodePath3TestAccess {
    static std::string extrude(GCode& generator, const ExtrusionPath& path)
    { return generator._extrude(path, "contour consumer test", 30.); }
    static void path(GCode& generator, ExtrusionPath path)
    { generator.extrude_path(std::move(path), "contour consumer test", 30.); }
    static void multipath(GCode& generator, ExtrusionMultiPath path)
    { generator.extrude_multi_path(std::move(path), "contour consumer test", 30.); }
    static std::string loop(GCode& generator, ExtrusionLoop loop)
    { return generator.extrude_loop(std::move(loop), "contour consumer test", 30.); }
    static bool has_wipe(const GCode& generator) { return generator.m_wipe.has_path(); }
    static double nominal_z(const GCode& generator) { return generator.m_nominal_z; }
    static void restore_volumetric_limit(GCode& generator)
    { generator.m_config.filament_max_volumetric_speed.values = {.4, .4, .4, .4}; }
    static void enable_loop_wipe(GCode& generator)
    {
        generator.m_config.wipe_on_loops.value = true;
        generator.m_config.wall_loops.value = 2;
        generator.m_config.enable_overhang_speed.values = {false};
    }
};
}

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void collect(const ExtrusionEntity& entity, std::vector<const ExtrusionPath*>& paths)
{
    if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            collect(*child, paths);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath& path : loop->paths) paths.push_back(&path);
    } else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath& path : multi->paths) paths.push_back(&path);
    } else if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        paths.push_back(path);
    }
}

struct Fixture {
    Model model;
    DynamicPrintConfig config;
    Print print;
    explicit Fixture(bool outward_side = false)
    {
        print.is_BBL_printer() = false;
        auto geometry = its_make_cube(10., 10., 2.);
        for (Vec3f& vertex : geometry.vertices)
            if (vertex.z() > 0)
                vertex.z() = 1.f + vertex.x() * 0.1f;
        if (outward_side)
            for (Vec3f& vertex : geometry.vertices)
                vertex.x() += 0.7f * vertex.z();
        auto* object = model.add_object();
        object->name = "contour-contract-wedge";
        object->add_volume(TriangleMesh(std::move(geometry)));
        object->add_instance();
        object->ensure_on_bed();
        const auto& defaults = FullPrintConfig::defaults();
        config.apply(static_cast<const PrintConfig&>(defaults), true);
        config.apply(static_cast<const PrintObjectConfig&>(defaults), true);
        config.apply(static_cast<const PrintRegionConfig&>(defaults), true);
        config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        config.set_key_value("zaa_enabled", new ConfigOptionBool(true));
        config.set_key_value("zaa_min_z", new ConfigOptionFloat(0.05));
        config.set_key_value("zaa_minimize_perimeter_height", new ConfigOptionFloat(0.));
        print.set_status_silent();
        print.set_cancel_callback([] {});
        apply();
    }
    void apply() { print.apply(model, config); print.restart(); }
    PrintObject& object() { return *print.get_object(0); }
    void variable_speed(bool enabled)
    {
        config.set_key_value("enable_overhang_speed", new ConfigOptionBools({static_cast<unsigned char>(enabled)}));
        config.set_key_value("slowdown_for_curled_perimeters", new ConfigOptionBools({false}));
        config.set_key_value("outer_wall_speed", new ConfigOptionFloats({60.}));
        config.set_key_value("inner_wall_speed", new ConfigOptionFloats({60.}));
        for (const char* key : {"overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed", "overhang_4_4_speed"})
            config.set_key_value(key, new ConfigOptionFloatsOrPercents({FloatOrPercent{5., false}}));
    }
    void require_variable_contour()
    {
        ExtrusionQualityEstimator estimator;
        const PrintObject* object = &this->object();
        estimator.set_current_object(object);
        const ConfigOptionPercents overlaps({90., 75., 50., 25., 13., 0.});
        const ConfigOptionFloatsOrPercents speeds({{100., true}, {5., false}, {5., false}, {5., false}, {5., false}, {5., false}});
        size_t variable_contours = 0;
        for (const Layer* layer : object->layers()) {
            estimator.prepare_for_new_layer(object, layer);
            if (layer->id() == 0) continue;
            std::vector<const ExtrusionPath*> paths;
            for (const LayerRegion* region : layer->regions()) collect(region->perimeters, paths);
            for (const ExtrusionPath* path : paths) {
                if (!path->has_polyline3() || !is_perimeter(path->role())) continue;
                const auto points = estimator.estimate_extrusion_quality(*path, overlaps, speeds, 60.f, 60.f, false);
                if (std::any_of(points.begin(), points.end(), [](const ProcessedPoint& point) { return point.speed < 59.f; })) {
                    Path3Projection projection(*path->polyline3());
                    for (const ProcessedPoint& point : points) projection.z(point.p);
                    ++variable_contours;
                }
            }
        }
        require(variable_contours > 0, "outward wedge did not exercise variable speed on contoured geometry");
    }
    std::vector<Points3> geometry(bool enabled)
    {
        const auto& object = this->object();
        require(object.is_step_done(posContouring), "native contour stage was not completed");
        require(object.is_step_done(posEstimateCurledExtrusions), "curled-extrusion stage was never completed");
        require(object.layers().size() > 3, "wedge fixture has too few layers");
        std::vector<Points3> snapshot;
        size_t nonzero = 0;
        for (size_t index = 0; index < object.layers().size(); ++index) {
            const Layer& layer = *object.layers()[index];
            const double model_top = layer.print_z - object.slicing_parameters().object_print_z_min;
            const double expected = enabled && index != 0 ? model_top - layer.height + config.opt_float("zaa_min_z") : model_top - 0.5 * layer.height;
            require(std::abs(layer.slice_z - expected) < 1.e-6, "contour slicing plane does not follow minimum Z");
            std::vector<const ExtrusionPath*> paths;
            for (const LayerRegion* region : layer.regions()) {
                collect(region->perimeters, paths);
                collect(region->fills, paths);
            }
            for (const ExtrusionPath* path : paths) {
                if (!enabled || index == 0)
                    require(!path->has_polyline3(), "disabled/first-layer contour has nonplanar paths");
                snapshot.push_back(path->points3());
                for (const Vec3crd& point : snapshot.back()) {
                    if (point.z() != 0) ++nonzero;
                    require(unscale<double>(point.z()) >= -layer.height - 1.e-6, "contour exceeds lower layer bound");
                    require(unscale<double>(point.z()) <= config.opt_float("zaa_min_z") + 1.e-6, "contour exceeds upper layer bound");
                }
            }
        }
        require(!snapshot.empty(), "wedge has no extrusion paths");
        require(enabled ? nonzero > 0 : nonzero == 0, "native contour did not produce the expected Z geometry");
        return snapshot;
    }
};
void export_spatial(Fixture& fixture, const test_fs::path& output)
{
    test_fs::create_directories(output);
    const auto gcode = output / "contour-wedge.gcode";
    GCodeProcessorResult processor;
    fixture.print.export_gcode(gcode.u8string(), &processor, nullptr);
    require(!processor.moves.empty(), "contoured paths did not reach native G-code export");
    std::ifstream stream(gcode);
    require(bool(stream), "contoured G-code was not written");
    bool spatial_extrusion = false;
    for (std::string line; std::getline(stream, line);)
        if (line.rfind("G1 ", 0) == 0 && line.find(" E") != std::string::npos &&
            line.find(" Z") != std::string::npos && (line.find(" X") != std::string::npos || line.find(" Y") != std::string::npos))
            spatial_extrusion = true;
    require(spatial_extrusion, "contour export contains no XYZ extrusion moves");
}

void consumer_boundaries(const test_fs::path& output)
{
    Fixture fixture;
    fixture.variable_speed(false);
    fixture.config.set_key_value("wipe", new ConfigOptionBools({true}));
    fixture.config.set_key_value("wipe_distance", new ConfigOptionFloats({2.}));
    fixture.apply();
    fixture.print.process();
    test_fs::create_directories(output);
    GCode generator;
    GCodeProcessorResult result;
    generator.do_export(&fixture.print, (output / "consumer-initialization.gcode").u8string().c_str(), &result);

    ExtrusionPath spatial(erExternalPerimeter, 0.08, 0.4f, 0.2f);
    spatial.set_polyline3(Polyline3(Points3{Vec3crd(scale_(1.), scale_(1.), scale_(-0.05)),
                                           Vec3crd(scale_(2.), scale_(1.), scale_(-0.05))}));
    GCodePath3TestAccess::path(generator, spatial);
    require(!GCodePath3TestAccess::has_wipe(generator), "contoured single path left a planar wipe path");

    ExtrusionPath flat(erExternalPerimeter, 0.08, 0.4f, 0.2f);
    flat.polyline = Polyline{spatial.last_point(), Point::new_scale(3., 1.)};
    ExtrusionPathSloped flat_scarf(flat, {1., 1.}, {1., 0.5});
    const std::string transition = GCodePath3TestAccess::extrude(generator, flat_scarf);
    bool restored_before_extrusion = false;
    std::istringstream lines(transition);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("G1 Z", 0) == 0)
            restored_before_extrusion = std::abs(std::stod(line.substr(4)) - GCodePath3TestAccess::nominal_z(generator)) < 0.001;
        if (line.rfind("G1 ", 0) == 0 && line.find(" E") != std::string::npos && line.find(" X") != std::string::npos)
            break;
    }
    require(restored_before_extrusion, "flat scarf at the same XY started at stale contour Z");
    GCodePath3TestAccess::path(generator, flat);
    require(GCodePath3TestAccess::has_wipe(generator), "ordinary planar single path lost wipe behavior");
    GCodePath3TestAccess::multipath(generator, ExtrusionMultiPath(spatial));
    require(!GCodePath3TestAccess::has_wipe(generator), "contoured multipath left a planar wipe path");
    GCodePath3TestAccess::multipath(generator, ExtrusionMultiPath(flat));
    require(GCodePath3TestAccess::has_wipe(generator), "ordinary planar multipath lost wipe behavior");

    spatial.set_polyline3(Polyline3(Points3{Vec3crd(scale_(1.), scale_(1.), scale_(-0.05)),
                                           Vec3crd(scale_(3.), scale_(1.), scale_(-0.05)),
                                           Vec3crd(scale_(3.), scale_(3.), scale_(-0.05)),
                                           Vec3crd(scale_(1.), scale_(1.), scale_(-0.05))}));
    GCodePath3TestAccess::enable_loop_wipe(generator);
    GCodePath3TestAccess::loop(generator, ExtrusionLoop(spatial));
    require(!GCodePath3TestAccess::has_wipe(generator), "contoured loop left a planar wipe path");
    require((generator.writer().get_position().head<2>() - generator.point_to_gcode(generator.last_pos())).norm() < 1.e-6,
            "contoured loop performed the unsupported planar inward wipe");

    Fixture local_z;
    local_z.config.set_num_extruders(4);
    local_z.config.set_num_filaments(4);
    local_z.config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.76, 1.77, 1.78}));
    local_z.config.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    local_z.config.set_key_value("mixed_filament_definitions", new ConfigOptionString("2,1,1,1,50,0,g,w,m0,z2,xa0,xb0,d0,o0,u3,cm3,r1/0.8000/0.2000"));
    local_z.config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    local_z.config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(true));
    local_z.config.set_key_value("dithering_local_z_infill", new ConfigOptionBool(true));
    local_z.config.set_key_value("mixed_filament_height_lower_bound", new ConfigOptionFloat(0.04));
    local_z.config.set_key_value("mixed_filament_height_upper_bound", new ConfigOptionFloat(0.16));
    local_z.config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    local_z.config.set_key_value("flush_into_infill", new ConfigOptionBool(false));
    local_z.config.set_key_value("flush_into_support", new ConfigOptionBool(false));
    local_z.model.objects.front()->config.set("extruder", 5);
    local_z.apply();
    local_z.print.process();
    local_z.geometry(true);
    const auto& plans = local_z.object().local_z_sublayer_plan();
    require(std::any_of(plans.begin(), plans.end(), [](const SubLayerPlan& plan) { return plan.split_interval; }),
            "Local-Z rejection fixture did not create micro-pass plans");
    bool rejected = false;
    const auto unsupported_output = output / "unsupported-local-z.gcode";
    try {
        GCodeProcessorResult local_result;
        local_z.print.export_gcode(unsupported_output.u8string(), &local_result, nullptr);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("Local-Z contour physical sublayer height") != std::string::npos;
    }
    require(rejected, "Local-Z contour outside physical layer bounds was not explicitly rejected");
    require(!test_fs::exists(unsupported_output), "rejected Local-Z export published a partial final G-code file");
    require(!local_z.print.is_step_done(psGCodeExport), "rejected Local-Z export was marked complete");
}

// Local-Z keeps original contour arc length and separately maps nozzle Z.
// These tests run the actual clip/pass/export path; there is no test-only
// production entry point or replacement writer.
struct LocalZMove {
    Vec3d from, to;
    double extrusion = 0.;
    double feedrate = 0.;
    unsigned tool = 0;
    bool spatial = false;
    bool local = false;
};
std::vector<LocalZMove> read_local_z_moves(const test_fs::path& path, const Vec3d& initial_position = Vec3d::Zero())
{
    std::ifstream stream(path);
    require(bool(stream), "Local-Z G-code was not written");
    Vec3d position = initial_position;
    double e = 0., feedrate = 0.;
    bool relative = true, local = false;
    unsigned tool = 0;
    std::vector<LocalZMove> moves;
    for (std::string line; std::getline(stream, line);) {
        if (line.find("local-z phase-b path passes begin") != std::string::npos) local = true;
        if (line.find("local-z phase-b path passes end") != std::string::npos) local = false;
        line.erase(line.find(';') == std::string::npos ? line.size() : line.find(';'));
        std::istringstream words(line);
        std::string command;
        if (!(words >> command)) continue;
        if (command == "M82") relative = false;
        if (command == "M83") relative = true;
        if (command.size() > 1 && command[0] == 'T' && std::isdigit(command[1])) tool = unsigned(std::stoul(command.substr(1)));
        if (command != "G0" && command != "G1" && command != "G2" && command != "G3" && command != "G92") continue;
        Vec3d target = position;
        double delta_e = 0.;
        bool xy = false, z = false;
        for (std::string word; words >> word;) {
            if (word.size() < 2 || std::string("XYZEF").find(word[0]) == std::string::npos) continue;
            const double value = std::stod(word.substr(1));
            if (word[0] == 'E') {
                if (command == "G92") e = value;
                else { delta_e = relative ? value : value - e; e = relative ? e + value : value; }
            } else if (word[0] == 'F') feedrate = value;
            else {
                const int axis = int(std::string("XYZ").find(word[0]));
                target[axis] = value;
                xy |= axis < 2; z |= axis == 2;
            }
        }
        if (command == "G92") continue;
        if (xy && delta_e > 0.) {
            require(!z || command == "G1", "Local-Z contour emitted a spatial arc");
            moves.push_back({position, target, delta_e, feedrate, tool, z, local});
        }
        position = target;
    }
    return moves;
}
void require_local_z_flow(const std::vector<LocalZMove>& moves)
{
    size_t spatial = 0;
    for (const auto& move : moves) if (move.local && move.spatial) {
        ++spatial;
        require(move.tool < 4, "Unexpected tool in Local-Z flow fixture");
        const double length = (move.to - move.from).norm();
        require(length > 0. && std::isfinite(length), "Positive contour extrusion has no serialized motion");
        const double diameter = 1.75 + .01 * move.tool;
        const double volume = move.extrusion * M_PI * diameter * diameter / 4.;
        require(volume * move.feedrate / (60. * length) <= .4 + 1e-9,
                "Serialized Local-Z contour exceeded filament volumetric limit");
    }
    require(spatial > 0, "Local-Z fixture did not emit a spatial extrusion");
}
void configure_local_z_contour(Fixture& fixture, bool relative = true)
{
    fixture.config.set_num_extruders(4);
    fixture.config.set_num_filaments(4);
    fixture.config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.76, 1.77, 1.78}));
    fixture.config.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}));
    fixture.config.set_key_value("mixed_filament_definitions", new ConfigOptionString("2,1,1,1,50,0,g,w,m0,z2,xa0,xb0,d0,o0,u3,cm3,r1/0.8000/0.2000"));
    fixture.config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    fixture.config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(true));
    fixture.config.set_key_value("dithering_local_z_infill", new ConfigOptionBool(true));
    fixture.config.set_key_value("mixed_filament_height_lower_bound", new ConfigOptionFloat(.01));
    fixture.config.set_key_value("mixed_filament_height_upper_bound", new ConfigOptionFloat(.3));
    fixture.config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    fixture.config.set_key_value("flush_into_infill", new ConfigOptionBool(false));
    fixture.config.set_key_value("flush_into_support", new ConfigOptionBool(false));
    fixture.config.set_key_value("use_relative_e_distances", new ConfigOptionBool(relative));
    fixture.config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats({.4, .4, .4, .4}));
    fixture.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats({1., 1., 1., 1.}));
    fixture.config.set_key_value("print_flow_ratio", new ConfigOptionFloat(1.));
    fixture.model.objects.front()->config.set("extruder", 5);
}
void test_local_z_exports(const test_fs::path& output)
{
    test_fs::create_directories(output);
    for (bool arc : {false, true}) for (bool speed : {false, true}) {
        Fixture fixture(speed);
        configure_local_z_contour(fixture);
        fixture.variable_speed(speed);
        fixture.config.set_key_value("enable_arc_fitting", new ConfigOptionBool(arc));
        fixture.apply(); fixture.print.process(); fixture.geometry(true);
        if (speed) fixture.require_variable_contour();
        const auto path = output / (std::string(arc ? "arc" : "linear") + (speed ? "-variable.gcode" : "-constant.gcode"));
        GCodeProcessorResult result;
        fixture.print.export_gcode(path.u8string(), &result);
        require(!result.moves.empty() && fixture.print.is_step_done(psGCodeExport), "Valid Local-Z contour export did not complete");
        const auto moves = read_local_z_moves(path);
        require_local_z_flow(moves);
        for (const auto& move : moves) if (move.local)
            require(move.to.z() >= .0995 && move.to.allFinite(), "Combined contour emitted invalid physical Z");
    }
}
void test_local_z_rejections(const test_fs::path& output)
{
    test_fs::create_directories(output);
    for (const std::string mode : {"lower", "upper", "ironing", "scarf", "adaptive"}) {
        Fixture fixture;
        configure_local_z_contour(fixture);
        if (mode == "lower") fixture.config.set_key_value("mixed_filament_height_lower_bound", new ConfigOptionFloat(.04));
        if (mode == "upper") fixture.config.set_key_value("mixed_filament_height_upper_bound", new ConfigOptionFloat(.16));
        if (mode == "ironing") fixture.config.set_deserialize_strict("ironing_type", "top");
        if (mode == "scarf") fixture.config.set_key_value("seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::All));
        if (mode == "adaptive") {
            fixture.config.set_key_value("adaptive_pressure_advance", new ConfigOptionBools({true, true, true, true}));
            fixture.config.set_key_value("enable_pressure_advance", new ConfigOptionBools({true, true, true, true}));
        }
        fixture.apply(); fixture.print.process(); fixture.geometry(true);
        const auto path = output / (mode + ".gcode");
        std::string failure;
        try { fixture.print.export_gcode(path.u8string(), nullptr); }
        catch (const std::exception& error) { failure = error.what(); }
        const std::string expected = mode == "lower" || mode == "upper" ? "physical sublayer height" : mode;
        require(failure.find(expected) != std::string::npos, "Unsupported Local-Z subset failed for an unrelated reason or was accepted");
        require(!test_fs::exists(path) && !fixture.print.is_step_done(psGCodeExport), "Rejected combination published final G-code or marked export complete");
    }
}
void test_local_z_writer(const test_fs::path& output)
{
    test_fs::create_directories(output);
    for (bool relative : {true, false}) {
        Fixture fixture;
        configure_local_z_contour(fixture, relative);
        fixture.variable_speed(false);
        fixture.apply(); fixture.print.process(); fixture.geometry(true);
        auto& object = fixture.object();
        Layer* layer = object.get_layer(1);
        require(layer && std::abs(layer->height - .2) < 1e-6, "Controlled layer height changed");
        const double bottom = layer->print_z - layer->height;
        std::vector<const SubLayerPlan*> plans;
        for (const auto& plan : object.local_z_sublayer_plan())
            if (plan.layer_id == size_t(layer->id()) && plan.split_interval) plans.push_back(&plan);
        require(plans.size() == 2, "Controlled fixture must use two unequal physical passes");
        // Keep the real plans and clipping masks, replace only deposited paths
        // by a known interior segment to make final XYZ and E independently predictable.
        for (Layer* current : object.layers()) for (LayerRegion* region : current->regions()) {
            region->perimeters.clear(); region->fills.clear();
        }
        ExtrusionPath known(erExternalPerimeter, .08, .4f, .2f);
        known.set_polyline3(Polyline3(Points3{Vec3crd(scale_(-2.), 0, scale_(-.10)), Vec3crd(scale_(2.), 0, scale_(.05))}));
        ExtrusionEntityCollection island;
        island.append(known);
        layer->regions().front()->perimeters.append(std::move(island));
        GCode generator;
        if (!relative) generator.set_gcode_offset(.123456789, -.987654321);
        const auto path = output / (relative ? "relative.gcode" : "absolute-offset.gcode");
        GCodeProcessorResult result;
        generator.do_export(&fixture.print, path.u8string().c_str(), &result);
        const auto moves = read_local_z_moves(path);
        require_local_z_flow(moves);
        size_t count = 0;
        double actual_volume = 0., expected_volume = 0.;
        for (const auto& move : moves) if (move.local && move.spatial) {
            require(count < plans.size(), "Controlled source segment emitted unexpected extra fragments");
            const auto* plan = plans[count++];
            const double fraction = (plan->print_z - bottom) / layer->height;
            const double from_offset = move.to.x() > move.from.x() ? -.10 : .05;
            const double to_offset = move.to.x() > move.from.x() ? .05 : -.10;
            require(std::abs(move.from.z() - (plan->print_z + fraction * from_offset)) < .0006,
                    "Controlled pass entry used thickness ratio instead of cumulative ratio");
            require(std::abs(move.to.z() - (plan->print_z + fraction * to_offset)) < .0006,
                    "Controlled pass changed final physical contour Z");
            require(std::abs(std::abs(move.to.x() - move.from.x()) - 4.) < .0011, "Controlled path was unexpectedly clipped");
            const double diameter = 1.75 + .01 * move.tool;
            const double area = M_PI * diameter * diameter / 4.;
            const double volume = move.extrusion * area;
            const double expected = .08 * plan->flow_height / layer->height * std::hypot(4., .15) * (1. + to_offset / layer->height);
            require(std::abs(volume - expected) <= area * 1.1e-5, "Controlled writer lost original 3D arc-length/height extrusion budget");
            actual_volume += volume; expected_volume += expected;
        }
        require(count == plans.size(), "Controlled path did not print every planned pass");
        require(std::abs(actual_volume - expected_volume) < 6e-5, "Controlled split volume mismatch");
        // A normal path after export must not inherit the last Local-Z scope.
        ExtrusionPath flat(erExternalPerimeter, .08, .4f, .2f);
        flat.polyline = Polyline{Point::new_scale(1., 1.), Point::new_scale(2., 1.)};
        const auto flat_path = output / (relative ? "flat-relative.gcode" : "flat-absolute.gcode");
        const Vec3d before_flat = generator.writer().get_position();
        std::ofstream(flat_path) << GCodePath3TestAccess::extrude(generator, flat);
        const auto flat_moves = read_local_z_moves(flat_path, before_flat);
        require(!flat_moves.empty(), "Post-export planar writer emitted no move");
        for (const auto& move : flat_moves)
            require(std::abs(move.to.z() - GCodePath3TestAccess::nominal_z(generator)) < .0006, "Post-export flat path inherited Local-Z contour context");
    }
}
void test_local_z_exception_restore(const test_fs::path& output)
{
    Fixture fixture;
    configure_local_z_contour(fixture);
    fixture.variable_speed(false);
    // Force an exception inside the real pass's spatial writer after nominal
    // Z has changed, rather than mocking the scoped context itself.
    fixture.config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats({1e-12, 1e-12, 1e-12, 1e-12}));
    fixture.apply(); fixture.print.process();
    double expected_nominal = 0.;
    for (const Layer* layer : fixture.object().layers()) {
        std::vector<const ExtrusionPath*> paths;
        for (const LayerRegion* region : layer->regions()) { collect(region->perimeters, paths); collect(region->fills, paths); }
        if (std::any_of(paths.begin(), paths.end(), [](const ExtrusionPath* path) { return path->has_polyline3(); })) {
            expected_nominal = layer->print_z; break;
        }
    }
    require(expected_nominal > 0., "Exception fixture has no actual spatial layer");
    GCode generator;
    const auto path = output / "precision-rejected.gcode";
    std::string failure;
    try { generator.do_export(&fixture.print, path.u8string().c_str(), nullptr); }
    catch (const std::exception& error) { failure = error.what(); }
    require(failure.find("below G-code feedrate precision") != std::string::npos && !test_fs::exists(path),
            "Real pass writer did not reject the unrepresentable feedrate transactionally");
    const double nominal = GCodePath3TestAccess::nominal_z(generator);
    require(std::abs(nominal - expected_nominal) < 1e-6,
            "Thrown pass did not restore the source layer nominal Z");
    GCodePath3TestAccess::restore_volumetric_limit(generator);
    ExtrusionPath ordinary(erExternalPerimeter, .08, .4f, .2f);
    ordinary.set_polyline3(Polyline3(Points3{Vec3crd(scale_(1.), scale_(1.), scale_(-.05)),
                                           Vec3crd(scale_(2.), scale_(1.), scale_(-.05))}));
    const auto recovered_path = output / "after-pass-exception.gcode";
    const Vec3d initial = generator.writer().get_position();
    std::ofstream(recovered_path) << GCodePath3TestAccess::extrude(generator, ordinary);
    const auto recovered = read_local_z_moves(recovered_path, initial);
    require(!recovered.empty(), "Post-exception contour emitted no native move");
    for (const auto& move : recovered)
        require(std::abs(move.to.z() - (nominal - .05)) < .0006,
                "Post-exception contour inherited a stale Local-Z cumulative ratio");
}

struct DetachHook {
    ~DetachHook() { set_slicing_pipeline_hook_fn(nullptr); }
};

}

void test_contour(const std::string& scenario, const test_fs::path& output)
{
    if (scenario == "contour_local_z") { test_local_z_exports(output); return; }
    if (scenario == "contour_local_z_rejections") { test_local_z_rejections(output); return; }
    if (scenario == "contour_local_z_writer") { test_local_z_writer(output); test_local_z_exception_restore(output); return; }
    if (scenario == "contour_consumers") {
        consumer_boundaries(output);
        return;
    }
    if (scenario == "contour_export_modes") {
        for (bool arc : {false, true}) for (bool speed : {false, true}) {
            Fixture fixture(speed);
            fixture.config.set_key_value("enable_arc_fitting", new ConfigOptionBool(arc));
            fixture.variable_speed(speed);
            fixture.apply();
            fixture.print.process();
            fixture.geometry(true);
            if (speed) fixture.require_variable_contour();
            export_spatial(fixture, output / (std::string(arc ? "arc" : "linear") + (speed ? "-variable" : "-constant")));
        }
        Fixture scarf(true);
        scarf.config.set_key_value("seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::All));
        scarf.config.set_key_value("seam_slope_conditional", new ConfigOptionBool(false));
        scarf.config.set_key_value("seam_slope_inner_walls", new ConfigOptionBool(true));
        scarf.config.set_key_value("seam_slope_start_height", new ConfigOptionFloatOrPercent(20., true));
        scarf.config.set_key_value("enable_arc_fitting", new ConfigOptionBool(true));
        scarf.variable_speed(true);
        scarf.apply();
        scarf.print.process();
        scarf.geometry(true);
        scarf.require_variable_contour();
        export_spatial(scarf, output / "scarf");
        return;
    }
    if (scenario == "contour_transform") {
        Fixture fixture;
        auto* object = fixture.model.objects.front();
        object->instances.front()->set_mirror(Vec3d(-1., 1., 1.));
        object->instances.front()->set_scaling_factor(Vec3d(1.4, 0.7, 1.2));
        object->instances.front()->set_rotation(Vec3d(0., 0., 0.31));
        object->ensure_on_bed();
        fixture.config.set_key_value("raft_layers", new ConfigOptionInt(2));
        fixture.apply();
        fixture.print.process();
        fixture.geometry(true);
        export_spatial(fixture, output);
        return;
    }
    if (scenario == "contour_lifecycle") {
        DetachHook detach;
        size_t completed = 0;
        set_slicing_pipeline_hook_fn([&](Print&, const PrintObject* object, SlicingPipelineStepPlugin step) {
            if (step == SlicingPipelineStepPlugin::posContouring) {
                require(object && object->is_step_done(posContouring), "contour hook ran before native completion");
                ++completed;
            }
        });
        Fixture fixture;
        fixture.print.process();
        fixture.geometry(true);
        require(completed == 1, "completed contour did not dispatch exactly one hook");
        fixture.print.process();
        require(completed == 1, "warm contour duplicated the completion hook");
        for (bool cancel : {false, true}) {
            Fixture interrupted;
            bool entered = false;
            const size_t before = completed;
            interrupted.print.set_status_callback([&](const PrintBase::SlicingStatus& status) {
                if (status.text == "Z contouring") {
                    entered = true;
                    if (cancel) interrupted.print.cancel();
                    else throw std::runtime_error("injected contour failure");
                }
            });
            bool failed = false;
            try { interrupted.print.process(); }
            catch (const CanceledException&) { failed = cancel; }
            catch (const std::runtime_error& error) { failed = !cancel && std::string(error.what()) == "injected contour failure"; }
            require(entered && failed, "native contour failure/cancellation was not exercised");
            require(!interrupted.object().is_step_done(posContouring), "failed contour stage was marked complete");
            require(completed == before, "failed contour dispatched a completion hook");
        }
        return;
    }
    Fixture fixture;
    fixture.print.process();
    const auto original = fixture.geometry(true);
    if (scenario == "contour_native") {
        fixture.print.process();
        require(fixture.geometry(true) == original, "unchanged contour geometry was applied twice");
        export_spatial(fixture, output);
    } else if (scenario == "contour_invalidation") {
        fixture.config.set_key_value("zaa_enabled", new ConfigOptionBool(false));
        fixture.apply();
        require(!fixture.object().is_step_done(posSlice), "disabling contour did not invalidate slicing");
        fixture.print.process();
        fixture.geometry(false);
        fixture.config.set_key_value("zaa_enabled", new ConfigOptionBool(true));
        fixture.config.set_key_value("zaa_min_z", new ConfigOptionFloat(0.06));
        fixture.apply();
        require(!fixture.object().is_step_done(posSlice), "changing contour minimum Z did not invalidate slicing");
        fixture.print.process();
        const auto changed = fixture.geometry(true);
        require(changed != original, "minimum Z change reused stale contour geometry");
        fixture.print.process(nullptr, true);
        require(fixture.geometry(true) == changed, "cache request changed or flattened contour geometry");
    } else {
        throw std::runtime_error("Unknown contour test scenario: " + scenario);
    }
}
