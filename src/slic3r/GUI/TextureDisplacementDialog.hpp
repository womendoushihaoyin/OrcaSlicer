#pragma once

#include "libslic3r/TriangleMesh.hpp"

#include <functional>
#include <optional>

class wxWindow;

namespace Slic3r::GUI {

// Modal, independent preview of a copied mesh. Uses the application's GL context on the UI thread;
// all CPU baking runs on a cancelable worker that is joined before the dialog is destroyed.
// The caller owns Undo/Redo and model replacement. Validate the captured object/volume identity
// and transform in `source_is_current` before committing the returned mesh.
// A baked mesh persists through the existing 3MF mesh path; this does not create editable layers.
std::optional<indexed_triangle_set> show_texture_displacement_dialog(
    wxWindow* parent, indexed_triangle_set source, bool flip_normals = false,
    const std::function<bool()>& source_is_current = {});

} // namespace Slic3r::GUI
