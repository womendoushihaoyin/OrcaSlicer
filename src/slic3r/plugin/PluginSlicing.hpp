#pragma once
#include "libslic3r/SlicingPipeline.hpp"

namespace Slic3r {
// Installs the per-print resolver. Missing/ambiguous preset references fail the
// slice before mutation; an empty selection retains the original native path.
void install_slicing_plugin_dispatcher();
}
