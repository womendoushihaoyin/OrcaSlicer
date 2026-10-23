#pragma once
#include "TextureEditSession.hpp"
class wxWindow;
namespace Slic3r::GUI {
bool edit_texture_uv(wxWindow* parent, TextureEditSession& session, int slot);
}
