// Layer data/persistence extracted from Orca 14a1aa333b6da3ecadf853a4a6285b0f193b7605.
// Shared by ModelVolume, Undo and the 3MF texture metadata reader/writer.
#ifndef slic3r_TextureDisplacementLayers_hpp_
#define slic3r_TextureDisplacementLayers_hpp_

#include <cmath>
#include <cstddef>
#include <Eigen/Core>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/array.hpp> // view_project_matrix is a std::array<float, 12>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

#include <array>

#include "Point.hpp"
#include "TextureDisplacementImage.hpp"

namespace Slic3r {

// Optional step-by-step capture of a bake; see TextureBake/TextureBakeDebug.hpp. Forward declared and
// taken by pointer so this header, which most of the texture feature includes, does not grow a
// dependency for something only the debug view and the benchmarks use.
class BakeStageRecorder;

class ModelVolume;

// Bits of subdivide_mesh_adaptive()'s per-triangle `refine_region` mask. See that function.
static constexpr uint8_t REFINE_PAINTED = 1;
static constexpr uint8_t REFINE_BORDER  = 2;

// Progress/cancellation hook for the (potentially multi-second) bake. Called with a 0..100
// percentage; return false to abort. See build_texture_displacement().
using DisplacementProgressFn = std::function<bool(int)>;

// Maximum number of simultaneous texture-displacement layers a single ModelVolume can hold.
// Each layer owns its own paint mask (ModelVolume::texture_displacement_facet(slot)), so this
// is also the number of independent EnforcerBlockerType selectors kept per volume.
static constexpr size_t TEXTURE_DISPLACEMENT_MAX_LAYERS = 8;

// How a layer's height texture is sampled outside its [0, 1) tile when tiling is enabled. Ignored
// (always clamp) when TextureDisplacementLayer::tile_enabled is false.
// How a layer's texture is mapped onto the mesh.
enum class TextureProjectionMethod : int
{
    // Standard *blended* tri-planar projection: the texture is sampled once per world axis (the
    // XY, XZ and YZ planes) and the three samples are blended per vertex, weighted by that
    // vertex's own normal raised to TRIPLANAR_BLEND_SHARPNESS.
    //
    // Earlier versions instead *hard-picked* the single axis most aligned with the normal. That
    // has a real, visible failure mode at any edge where the dominant axis flips: on a +X face the
    // planar coordinate is (y, z), on a -Y face it is (x, z), so at the shared edge u jumps from
    // y_edge to x_edge. On a box centred near the origin those two agree at the (+,+) and (-,-)
    // corners (making them look fine) but differ by the full corner width at the (+,-) and (-,+)
    // corners, which is exactly the "two bad corners, two good ones" seam that was reported.
    // Blending across the transition removes that hard discontinuity by construction.
    Triplanar   = 0,
    // Wrapped around an axis running through the patch's centroid. The axis itself is picked
    // automatically as the world axis *least* aligned with the patch's average normal (since a
    // cylinder's own axis is perpendicular to its outward radial normal) - a reasonable default
    // for roughly cylindrical selections, not a precise fit for arbitrary geometry.
    Cylindrical = 1,
    // Wrapped around the patch's centroid using longitude/latitude - reasonable for roughly
    // spherical/rounded selections, again an approximation rather than an exact geodesic map.
    Spherical   = 2,
    // Real UV unwrap of the painted patch - a proper low-distortion flattening rather than a
    // planar/cylindrical/spherical approximation. The patch is first cut into charts along its
    // sharp edges and each chart is flattened on its own (see compute_patch_unwrap()), so a patch
    // that is not a single developable surface still unwraps sensibly. Falls back to Triplanar for
    // any chart that cannot be flattened at all.
    LSCM        = 3,
    // Flat projection along a fixed direction captured from the 3D camera ("project from view"): the
    // texture is laid onto the painted area as seen from that angle, like a decal projector. Single
    // planar map (no per-face axis switch), so it can smear on faces turned away from the projector -
    // that is inherent to view projection and is the user's call, not a bug. The projector's two
    // in-plane axes live in TextureDisplacementLayer::view_project_right/up.
    ViewProjected = 4,
    // The model's own texture coordinates, as the file it was imported from carried them (see
    // ModelVolume::texture_uv). Nothing is unwrapped or projected: the artist's layout *is* the
    // placement, so a texture authored against that layout - the model's own albedo map, say - lands
    // exactly where it was painted. Automatically picked for a new layer on a volume that has a UV
    // channel, since re-unwrapping a model that already carries one can only be worse than using it.
    //
    // The uv is already normalized, so unlike every method above it is not measured in millimetres:
    // "Tile size" and the non-square aspect correction do not apply to it (the layer's rotation and
    // offset still do). Falls back to Triplanar when the volume has no channel, exactly as LSCM falls
    // back when a patch cannot be flattened.
    ModelUV       = 5,
};

// Dihedral angle (degrees) above which an edge between two painted triangles becomes a chart seam
// - i.e. the unwrap is cut there rather than being forced to flatten across it.
//
// The whole point of this being a threshold rather than "flatten everything as one piece": three
// faces meeting at a cube corner are not developable, so a single-chart solve has to distort them
// badly to lie flat (they splay out into a fan, which is what "it merges all the edges into a
// triangle" describes). Cutting at the 90-degree edges instead lets each face flatten exactly.
// Meanwhile a smoothly curved surface - a subdivided sphere, say - has only small angles between
// neighbouring triangles, stays a single chart, and unwraps as one piece the way it should.
static constexpr float LSCM_DEFAULT_SEAM_ANGLE_DEG = 30.f;

// Exponent the tri-planar blend weights are raised to (see TextureProjectionMethod::Triplanar).
// Higher means a tighter, more "hard-edged" transition between the three axis projections; lower
// means a wider cross-fade. 4 is the usual default: tight enough that a flat face is sampled
// almost purely along its own axis, wide enough that a 90-degree edge has no visible hard seam.
static constexpr float TRIPLANAR_BLEND_SHARPNESS = 4.f;

// How a layer's displacement combines with the displacement accumulated by the layers below it
// (i.e. those in lower slots), evaluated per vertex. Analogous to an image editor's layer blend
// modes, except the quantity being blended is a signed displacement distance in mm rather than a
// pixel value.
//
// Add/Subtract are in mm and need no further explanation. Multiply/Divide are *scaling* operations
// and therefore need a unit convention: they treat the layer's own value as a unitless factor
// relative to 1 mm. That makes `depth_mm` act as a gain - a layer with depth 1 mm and a white
// (1.0) texel multiplies the accumulated relief by exactly 1, i.e. leaves it unchanged - which is
// the behaviour that makes a Multiply layer usable as a mask over the layers beneath it.
enum class TextureBlendMode : int
{
    Add      = 0, // acc + value          (default; several layers pile their relief up together)
    Subtract = 1, // acc - value          (carve this layer's relief out of the layers below)
    Multiply = 2, // acc * (value / 1mm)  (mask/modulate the layers below by this layer)
    Divide   = 3, // acc / (value / 1mm)  (inverse mask; guarded against a zero/near-zero divisor)
    // acc = value: this layer replaces what is under it instead of combining with it, so where it is
    // painted only its own relief remains. The image editor's "Normal" mode, under the name the panel
    // uses. Note the stack's *colour* already behaves this way - a higher slot's colour overwrites a
    // lower one's where they overlap - so this is what makes the displacement agree with it.
    Override = 4,
};

// Combines one layer's signed displacement `value` (mm) into `accumulated` (mm) per `mode`.
// Shared by the bake/preview path and exposed for tests.
float blend_displacement(float accumulated, float value, TextureBlendMode mode);

// Where one unwrap island (chart) sits in UV space, on top of wherever compute_patch_unwrap() first
// packed it. This is what the UV editor's drag/rotate gestures write to, so a user can lay the
// islands out by hand - move them, rotate them, overlap them - rather than being stuck with the
// automatic packing.
//
// Indexed by chart id, which compute_patch_unwrap() assigns in first-encountered-triangle order. That
// is stable for a given patch and seam angle, but *not* across a change to either: repainting the
// patch, or moving the seam-angle slider, can renumber the charts and so leave a hand-placed island
// applied to a different one. Accepted deliberately - the alternative is a persistent chart identity
// that survives arbitrary re-segmentation, which is a much larger problem than this feature warrants.
struct TextureIsland
{
    Vec2f offset       = Vec2f::Zero(); // in the unwrap's own mm space
    float rotation_deg = 0.f;           // about the island's own centroid
    // About the island's own centroid too. 1 = the size compute_patch_unwrap() gave it, which is
    // already its true surface area in mm - so scaling an island away from 1 deliberately makes its
    // texel density differ from its neighbours'. See average_island_scales().
    float scale = 1.f;

    template<class Archive> void serialize(Archive &ar) { ar(offset, rotation_deg, scale); }
};

// Sets every island's scale to the mean of the current ones (Blender's "Average Islands Scale").
// Only meaningful after islands have been scaled by hand: compute_patch_unwrap() already sizes every
// chart to its true mm area, so a freshly unwrapped patch has uniform texel density to begin with.
void average_island_scales(std::vector<TextureIsland> &islands);

// Photo-style adjustments to a layer's source image, as the gizmo's image-adjustment panel sets them.
//
// These are *not* layer parameters: nothing here is stored on TextureDisplacementLayer and nothing here is
// re-applied at bake time. The panel applies them once, to the image, and the result becomes the layer's
// image - so the adjusted pixels feed the colour and the displacement together, and a brightness change
// really does change the relief. That is the intended behaviour: one image, one source of truth, rather
// than a second set of knobs the bake would have to replay in the right order.
//
// Contrast with TextureDisplacementLayer::smoothing, which *is* a layer parameter applied on every decode:
// `blur` here is a destructive edit of the image, `smoothing` is a live low-pass over whatever the image is.
struct TextureAdjustments
{
    float brightness = 0.f; // [-1, 1]; 0 unchanged, -1 black, +1 white
    float contrast   = 0.f; // [-1, 1]; 0 unchanged, -1 flat mid-grey, +1 hard threshold
    float sharpness  = 0.f; // [0, 1];  0 unchanged. Unsharp-mask amount
    float blur       = 0.f; // [0, 1];  0 unchanged. Fraction of SOURCE_BLUR_SPAN, as `smoothing` is
    float hue_deg    = 0.f; // [-180, 180]; 0 unchanged. No effect on a grayscale image - grey has no hue

    bool identity() const
    {
        return brightness == 0.f && contrast == 0.f && sharpness == 0.f && blur == 0.f && hue_deg == 0.f;
    }
    // Exact equality, which is what is wanted: these come straight off sliders, and the question asked of
    // them is "is this still the value the preview on screen was computed from", not "are these close".
    bool operator==(const TextureAdjustments &o) const
    {
        return brightness == o.brightness && contrast == o.contrast && sharpness == o.sharpness &&
               blur == o.blur && hue_deg == o.hue_deg;
    }
    bool operator!=(const TextureAdjustments &o) const { return !(*this == o); }
};

// One texture asset plus its projection/displacement parameters. Several layers may be painted
// onto overlapping areas of the same volume: their displacements are combined per vertex, in slot
// order, each layer folding into the total via its own TextureBlendMode (see
// build_texture_displacement()). This is what "layered/blended" texture displacement means here.
struct TextureDisplacementLayer
{
    // Index into ModelVolume::texture_displacement_facets, assigned once when the layer is
    // created. Not reused for the lifetime of the ModelVolume, so a deleted layer's slot simply
    // becomes unused rather than being handed to a different layer.
    int slot = -1;

    std::string name;
    // Path on the local filesystem the image was loaded from (informational; may be stale or
    // empty, e.g. after loading a .3mf on a different machine).
    std::string path;
    // Path inside the .3mf archive once saved (empty until the project is saved once).
    std::string path_in_3mf;
    // Raw encoded image bytes. Only 8-bit grayscale PNG is understood by decode_height_texture()
    // (libslic3r has no GUI image toolkit available); the GUI converts any imported image to that
    // format before storing it here, so the baking code never needs to depend on wxWidgets.
    std::shared_ptr<std::vector<unsigned char>> image_data;

    float depth_mm     = 0.4f; // maximum displacement along the surface normal, in mm
    float tiling_scale = 12.5f; // size of one texture tile, in mm
    float rotation_deg = 0.f;
    Vec2f offset       = Vec2f::Zero();
    bool  invert       = false;

    // The height value that means "don't move this vertex". The sampled height (0..1) has this
    // subtracted before being scaled by depth_mm, so with the default of 0 the surface only ever
    // moves *outwards* (the classic height-map convention), while 0.5 makes mid-grey neutral and
    // lets darker texels cut *into* the surface - an engraved-and-embossed result from one map.
    //
    // Cutting inward is not free: vertices move along their own normals, which converge inside a
    // concave corner and inside a thin wall, so a large depth_mm against a small feature really can
    // fold the surface through itself. There is no cheap way to detect that here (it needs a full
    // self-intersection test on the displaced mesh), so the GUI warns rather than promising safety.
    float midlevel = 0.f;

    // Optional blur applied to the decoded height map before it is sampled, in [0, 1]: 0 is the raw
    // texture, 1 the strongest blur. Softens the relief (rounds hard edges, removes speckle) without
    // needing a pre-blurred source image. Applied in decode_height_texture(), so it feeds the true
    // preview, the UV editor backdrop and the bake identically.
    float smoothing = 0.f;

    // Optional feathering of the displacement toward the edge of the painted patch. When enabled, the
    // displacement is scaled down as a vertex approaches the patch boundary, so the relief blends
    // smoothly into the surrounding surface instead of ending abruptly. `edge_smoothing_amount` in
    // (0, 1] sets how far the fade reaches into the patch: small values only soften a thin band at the
    // very edge, 1 fades the whole patch to nothing (the painted face comes out flat). Off by default.
    bool  edge_smoothing        = false;
    float edge_smoothing_amount = 0.5f;

    // Only used by TextureProjectionMethod::LSCM: when set, a fresh unwrap is laid out as a connected
    // net (adjacent charts unfolded edge-to-edge along a spanning tree, see compute_connected_net())
    // rather than as separately packed islands. On by default. Hand-moving an island overrides its
    // placement until the next re-unwrap.
    bool  auto_connect_islands  = true;

    // When false, the texture is sampled once (clamped to its edge pixels outside [0, 1)) instead
    // of being repeated - useful for a single decal-like placement rather than a repeating tile.
    bool              tile_enabled = true;
    TextureTileMethod tile_method  = TextureTileMethod::Repeat;

    TextureProjectionMethod projection_method = TextureProjectionMethod::Triplanar;
    // Only used by TextureProjectionMethod::LSCM. See LSCM_DEFAULT_SEAM_ANGLE_DEG.
    float                   lscm_seam_angle_deg = LSCM_DEFAULT_SEAM_ANGLE_DEG;
    // Only used by TextureProjectionMethod::LSCM: gap left between islands by the automatic packing,
    // in the unwrap's mm space. Negative means "auto" (a small fraction of the packed size), which is
    // what a patch that has never had the slider touched gets.
    float                   island_padding_mm = -1.f;
    // Only used by TextureProjectionMethod::LSCM: edges the unwrap is forced to cut along, on top of
    // whatever the seam angle already cuts. Each pair is an undirected edge in *mesh vertex index*
    // space (first < second). This is what "mark seam" (manual) and "cut island" (auto) both write to.
    // Mesh-index space, so like the paint masks these are dropped on any topology change.
    std::vector<std::pair<int, int>> lscm_seam_edges;

    // Only used by TextureProjectionMethod::ViewProjected: the projector's in-plane axes, in the
    // volume's *local* space, captured from the camera when the user hits "Project from view". A point
    // projects to Vec2f(dot(pos, right), dot(pos, up)) before the usual tiling/rotation/offset.
    Vec3f view_project_right = Vec3f::UnitX();
    Vec3f view_project_up    = Vec3f::UnitY();

    // Also ViewProjected, and takes precedence over the two axes above when set: an exact *projective*
    // map from a local-space position straight to a texture uv, written by the projection-frame overlay
    // (the semi-transparent window dragged over the 3D view - its border becomes the uv unit square).
    //
    // Row-major 3x4, applied to the homogeneous point p~ = (x, y, z, 1):
    //     uv = ( row0.p~ / row2.p~ , row1.p~ / row2.p~ )
    // The perspective divide is the whole point. view_project_right/up can only express an *affine*
    // projection, which matches an orthographic camera exactly but not a perspective one - under
    // perspective the near end of a part projects larger than the far end, and no pair of axes
    // reproduces that. Folding the camera's full projection*view*model product into one matrix does.
    // Because a point behind the projector has row2.p~ <= 0 and no meaningful uv, sampling must check
    // the sign rather than divide blindly; see project_uv_projective().
    //
    // Note this map already includes placement, so the usual tiling/rotation/offset transform is NOT
    // applied on top of it - the window's own position and size are the placement.
    bool                  view_project_projective = false;
    std::array<float, 12> view_project_matrix{};
    // Only used by TextureProjectionMethod::LSCM: hand placement of the unwrap's islands, indexed by
    // chart id (see TextureIsland). Shorter than the chart count simply means the missing ones are
    // still where the automatic packing put them.
    std::vector<TextureIsland> islands;

    // Only used by TextureProjectionMethod::LSCM: persistent "join" groups, indexed by chart id. Charts
    // that share a group id move together as one in the UV editor - this is what the explicit "Join"
    // command records (over and above placing the child next to its parent). An entry of -1, or an index
    // past the end of the vector, means the chart is its own singleton group (moves alone). Empty means
    // every chart is a singleton. Same chart-renumbering caveat as `islands`: a re-unwrap can reshuffle
    // chart ids, so this is meaningful only against the unwrap it was made on.
    std::vector<int> island_groups;

    // Only used by TextureProjectionMethod::LSCM: manual UV edits made in the UV editor's Vertex/Edge
    // select modes. Each pair is (key, its overriding raw-unwrap coordinate in mm) - the *raw* unwrap
    // position, i.e. before the island transform, so the edited vertex still moves and rotates with its
    // island. The key names what is edited (see apply_lscm_uv_overrides()):
    //  - negative: one unwrapped vertex of the current unwrap, as lscm_uv_override_key(index). This is what
    //    the editor stores, so a seam vertex dragged in one island leaves its copies in the neighbouring
    //    islands where they are. Like `islands`, meaningful only against the unwrap it was made on.
    //  - zero or positive: a mesh vertex, every unwrapped copy of it (how edits were stored before).
    std::vector<std::pair<int, Vec2f>> lscm_uv_overrides;

    // Live, unsaved image adjustments from the gizmo's image-adjustment panel, applied by
    // decode_height_texture() on top of the decoded image - so while the panel is open the relief, the
    // colour, the thumbnails and the bake all see the adjusted image, and moving a slider really does
    // change the displacement. See TextureAdjustments.
    //
    // Deliberately *not* serialized, by either save()/load() here or the .3mf: it is the state of an open
    // editor, not a property of the layer. The panel resets it when it closes, and "Save as copy" resolves
    // it by writing a new image and pointing the layer at that - which is the only way an adjustment
    // becomes permanent. A project saved mid-adjustment therefore stores the unadjusted image, which is
    // the honest answer: nothing was committed.
    TextureAdjustments preview_adjust;

    // How this layer folds into the displacement accumulated by the layers below it. Ignored for
    // the lowest-slot painted layer, which has nothing beneath it to combine with (the GUI shows
    // it as the "Base" layer and hides the control).
    TextureBlendMode blend_mode = TextureBlendMode::Add;

    // Colour this layer's painted area from the texture's own colours, on top of displacing by its
    // height. Only meaningful when the texture actually has colour (DecodedHeightTexture::has_color()):
    // the shipped library is grayscale, so this does nothing there.
    //
    // Colour lands in the volume's mmu_segmentation_facets - the same per-triangle filament assignment
    // the MMU paint gizmo writes - so its resolution is the *mesh's*, not the image's, and a triangle
    // gets exactly one filament. That is why the adaptive subdivision has a colour criterion of its
    // own (see subdivide_mesh_adaptive()): without triangles along a colour boundary there is nothing
    // for the boundary to be drawn on.
    bool color_enabled = false;

    bool empty() const { return !image_data || image_data->empty(); }

    template<class Archive> void save(Archive &ar) const
    {
        std::string blob = image_data ? std::string(image_data->begin(), image_data->end()) : std::string();
        ar(slot, name, path, path_in_3mf, blob, depth_mm, tiling_scale, rotation_deg, offset, invert, tile_enabled,
           static_cast<int>(tile_method), static_cast<int>(projection_method), lscm_seam_angle_deg, islands,
           static_cast<int>(blend_mode), midlevel, island_padding_mm, lscm_seam_edges, view_project_right,
           view_project_up, smoothing, edge_smoothing, edge_smoothing_amount, auto_connect_islands, island_groups,
           lscm_uv_overrides, view_project_projective, view_project_matrix, color_enabled);
    }
    template<class Archive> void load(Archive &ar)
    {
        std::string blob;
        int         tile_method_int       = 0;
        int         projection_method_int = 0;
        int         blend_mode_int        = 0;
        ar(slot, name, path, path_in_3mf, blob, depth_mm, tiling_scale, rotation_deg, offset, invert, tile_enabled,
           tile_method_int, projection_method_int, lscm_seam_angle_deg, islands, blend_mode_int, midlevel,
           island_padding_mm, lscm_seam_edges, view_project_right, view_project_up, smoothing, edge_smoothing,
           edge_smoothing_amount, auto_connect_islands, island_groups, lscm_uv_overrides, view_project_projective,
           view_project_matrix, color_enabled);
        image_data        = blob.empty() ? nullptr : std::make_shared<std::vector<unsigned char>>(blob.begin(), blob.end());
        tile_method       = static_cast<TextureTileMethod>(tile_method_int);
        projection_method = static_cast<TextureProjectionMethod>(projection_method_int);
        blend_mode        = static_cast<TextureBlendMode>(blend_mode_int);
    }
};


// Settings that apply to the whole layer stack rather than to one layer, held per ModelVolume next
// to texture_displacement_layers and consumed by build_texture_displacement().
struct TextureDisplacementOptions
{
    // Whether the painted patch's *border* vertices - the ones also used by unpainted triangles -
    // are displaced along with the rest, or pinned flat.
    //
    // Pinning them was originally justified as keeping the patch from tearing away from the
    // surrounding surface. That reasoning no longer applies: since the bake became
    // topology-preserving it only ever *moves* the input's own vertices, so a border vertex is one
    // vertex shared by both regions and moving it simply tilts the unpainted triangles that use it -
    // nothing can come apart. What pinning actually does is clamp the outermost ring of the relief to
    // zero, which on a fully painted face collapses the pattern into a ring of steep ramps right at
    // the edge (the "it doesn't extrude at the border" artifact). Displacing it is the default;
    // pinning is kept for the case where the relief must not spill past the paint at all.
    bool displace_border = true;

    // Optional Laplacian relaxation of the displaced surface, run after all layers have been folded
    // in - a post-process, not a texture filter (TextureDisplacementLayer::smoothing blurs the height
    // map instead, before it is ever sampled). Rounds off the hard steps a bitmap height map leaves
    // behind. Restricted to vertices the displacement actually moved, so the rest of the model keeps
    // its exact geometry. `smooth_strength` in [0, 1] is how far each pass moves a vertex toward the
    // average of its neighbours.
    bool  smooth_enabled    = false;
    float smooth_strength   = 0.3f;
    int   smooth_iterations = 2;

    // Hold the painted patch's outermost ring of vertices out of the smoothing. Those vertices sit
    // next to unpainted ones that are pinned by definition, so relaxing them drags the rim of the
    // relief back down toward the undisplaced surface - the pattern looks half-melted exactly where it
    // meets the edge, however crisp the rest of it is. Excluding them keeps the border extruded at
    // full depth and smooths only the interior. On by default; turn it off to soften the outer edge
    // deliberately (which is a blunter version of the per-layer edge-smoothing falloff).
    bool  smooth_skip_border = true;

    // Which bake pipeline. On (the default): refine, remove slivers, displace and simplify in one run,
    // nothing to prepare first. Off, the classic path: the mesh is prepared first (remesh, adaptive
    // subdivision, step cut) and then displaced vertex by vertex, keeping the topology - which is what
    // colour needs, since the per-facet assignment has nothing stable to attach to once the topology is
    // rebuilt. Projects saved with the classic path keep it: the flag is stored per volume.
    bool  pipeline_v2        = true;
    // Refinement edge length, mm. 0 (the default) means automatic: chosen from the texture's texel
    // size and sharpness and the model's size, see recommend_v2_resolution(). A saved project with an
    // explicit value keeps it.
    float v2_refine_mm       = 0.f;
    bool  v2_regularize      = false;
    // Simplification target in thousands of triangles. -1 (the default) means automatic, from the
    // same recommendation; 0 skips simplification, which is worth comparing on its own.
    int   v2_max_triangles_k = -1;
    // Slide vertices onto the texture's own edges before displacing. Displacement moves vertices along
    // the normal only, so without this a step in the image is reproduced wherever the triangle grid
    // happens to fall, as a staircase rather than a straight wall.
    bool  v2_relocate          = false;
    // Data-dependent edge flips before displacement (see TextureBakeFlip.hpp). On by default and not a
    // user setting; deliberately left out of serialize() so project files are unaffected.
    bool  v2_flip_edges        = true;

    // Colour, all of which belongs to the stack rather than to any one layer: it is about how the
    // printer will realise the colours, not about which image they came from.

    // Interleave pairs of filaments to get colours between them - so four loaded filaments offer far
    // more than four colours. Whether a given layer's colours actually use mixes is decided from its
    // image (TextureDetail::flat_colors): a texture of flat colours prints in single filaments, a
    // photograph or gradient in mixes. Off forces single filaments everywhere.
    bool         color_mix_enabled = true;
    // The most mixes the palette may offer. Every mix a bake paints with becomes a mixed filament slot,
    // so this is also the most slots one bake can add. The mixes themselves are picked from the
    // texture's colours, those that improve the match the most coming first.
    int          color_mix_count   = 8;
    // Majority-filter passes over the assigned colours. See TextureColorRequest::despeckle_passes -
    // this is the control for it, and 2 is enough to clear the salt-and-pepper an image with detail
    // finer than the mesh leaves behind, without eating features that are genuinely a facet wide.
    int          color_despeckle   = 2;

    // Where the panel was left, rather than anything the bake reads. It lives with the volume because
    // that is what it describes: reopening a project should put the user back on the layer they were
    // editing, in the mode they were editing it in, not on slot 0 in Standard.
    int active_slot = 0;
    int panel_mode  = 0; // 0 Standard, 1 Pro

    template<class Archive> void serialize(Archive &ar)
    {
        ar(displace_border, smooth_enabled, smooth_strength, smooth_iterations, smooth_skip_border,
           pipeline_v2, v2_refine_mm, v2_regularize, v2_max_triangles_k,
           v2_relocate, v2_flip_edges, color_mix_enabled, color_despeckle, color_mix_count,
           active_slot, panel_mode);
    }
};

// Project persistence (see bbs_3mf.cpp). The layer stack and the per-volume options are written to the
// .3mf as JSON rather than through the cereal save()/load() above: those two are positional and
// unversioned, which is right for the undo/redo stack they serve (one session, one binary) but would
// make every future field a project-breaking change. A JSON object tolerates both directions - an
// unknown key is ignored, a missing one keeps the member's default - so old projects keep loading and
// new ones degrade gracefully in older builds.
//
// The texture image itself is *not* in here. It is a binary blob that belongs in the archive as a file
// of its own, exactly as EmbossShape stores its SVG; `path_in_3mf` names that file, and the caller is
// responsible for writing it and for filling `image_data` back in on load.
std::string texture_displacement_layers_to_json(const std::vector<TextureDisplacementLayer> &layers,
                                                const TextureDisplacementOptions            &options);
// Returns false and leaves both outputs untouched when the text is not a valid JSON object, a number in a
// layer's vector, matrix, seam, island or UV-override array has the wrong type, or a layer's slot is
// missing or outside [0, TEXTURE_DISPLACEMENT_MAX_LAYERS). Any other value of the wrong type or shape
// keeps its default.
bool texture_displacement_layers_from_json(const std::string                      &text,
                                           std::vector<TextureDisplacementLayer>  &layers,
                                           TextureDisplacementOptions             &options);


} // namespace Slic3r
#endif
