#include "slic3r/GUI/TextureEditHistory.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
void test_texture_history(const std::filesystem::path& fixtures)
{
    Model model; auto* object = model.add_object(); object->add_instance();
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0,0,0),Vec3f(10,0,0),Vec3f(10,10,0),Vec3f(0,10,0)};
    mesh.indices = {Vec3i32(0,1,2),Vec3i32(0,2,3)};
    auto* volume = object->add_volume(TriangleMesh(mesh));
    TextureEditSession state(*volume);
    TextureEditHistory history(state);
    require(!history.can_undo() && !history.can_redo(), "Fresh modal history is not empty");
    std::ifstream input(fixtures / "texture-rgb.png", std::ios::binary);
    require(bool(input), "Missing history image fixture");
    auto bytes = std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    state.add_layer("RGB", "memory://rgb", bytes); history.record(state, "Add layer");
    require(history.undo(state) && state.layers.empty(), "Layer add cannot be undone");
    require(history.redo(state) && state.layers.size() == 1 && state.layer(0)->image_data == bytes,
            "Layer redo lost owned image or metadata");
    const float before = state.layer(0)->depth_mm;
    state.layer(0)->depth_mm = 1.f; history.record(state, "Depth", 17);
    state.layer(0)->depth_mm = 2.f; history.record(state, "Depth", 17);
    state.layer(0)->depth_mm = 3.f; history.record(state, "Depth", 17);
    require(history.undo(state) && state.layer(0)->depth_mm == before,
            "One numeric edit became multiple undo steps");
    require(history.redo(state) && state.layer(0)->depth_mm == 3.f, "Numeric redo missed the final value");
    history.end_group();
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0,EnforcerBlockerType::ENFORCER); state.masks[0] = selector.serialize();
    selector.set_facet(1,EnforcerBlockerType::ENFORCER); state.masks[0] = selector.serialize();
    history.record(state,"One complete stroke");
    const auto stroke = state.masks[0];
    require(history.undo(state) && !TriangleSelector::has_facets(state.masks[0],EnforcerBlockerType::ENFORCER),
            "Whole-stroke undo left a partial painted mask");
    require(history.redo(state) && state.masks[0] == stroke, "Stroke redo lost exact selector data");
    state.model_uv.assign(6,Vec2f(.2f,.7f)); state.layer(0)->islands.resize(1);
    state.layer(0)->islands[0].rotation_deg = 30.f;
    history.record(state,"UV edit");
    require(history.undo(state) && state.model_uv.empty() && state.layer(0)->islands.empty(),
            "UV transaction was not undone atomically");
    require(history.redo(state) && state.model_uv.size() == 6 && state.layer(0)->islands[0].rotation_deg == 30.f,
            "UV redo lost island or corner data");
    require(!history.record(state,"No-op") && history.can_undo(), "No-op edit made a history entry");
    require(history.undo(state), "Cannot undo before branch edit");
    state.layer(0)->invert = true; history.record(state,"Invert");
    require(!history.can_redo(), "A new edit retained an invalid redo branch");
    bytes.reset();
    require(!state.layer(0)->image_data->empty() && state.source == volume->mesh_ptr(),
            "History did not retain owned image or shared source lifetime");
    require(volume->texture_displacement_layers.empty() && volume->texture_uv.empty(),
            "Modal undo history modified the live model");
    TextureEditHistory bounded(state,2);
    for (int i = 0; i < 5; ++i) { state.layer(0)->rotation_deg = float(i); bounded.record(state,"Rotate"); }
    require(bounded.undo(state) && bounded.undo(state) && !bounded.undo(state), "History limit was not enforced");
}
