#include "libslic3r/TextureDisplacementPaint.hpp"
#include "EditableTextureUV.hpp"
#include "TextureEditingTools.hpp"
#include "TextureUVSelection.hpp"
#include "TextureIslandGroups.hpp"
#include "TextureIslandLayout.hpp"
#include "TextureIslandCut.hpp"
#include "I18N.hpp"
#include <algorithm>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/bitmap.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/spinctrl.h>

namespace Slic3r::GUI {
namespace {

// A chart is a connected UV island. Connectivity requires the model edge and
// both UV endpoints to agree, so an imported seam never joins across its cut.
PatchUnwrap model_uv_layout(const indexed_triangle_set& mesh, const std::vector<Vec2f>& uv)
{
    PatchUnwrap layout;
    layout.uvs = uv;
    const size_t count = mesh.indices.size();
    std::vector<int> parent(count);
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](int node) {
        while (parent[size_t(node)] != node) {
            parent[size_t(node)] = parent[size_t(parent[size_t(node)])];
            node = parent[size_t(node)];
        }
        return node;
    };
    using Edge = std::tuple<int, int, float, float, float, float>;
    std::map<Edge, int> edges;
    for (size_t face = 0; face < count; ++face) {
        for (int corner = 0; corner < 3; ++corner) {
            int a = corner, b = (corner + 1) % 3;
            if (mesh.indices[face][a] > mesh.indices[face][b])
                std::swap(a, b);
            const Vec2f& ua = uv[face * 3 + size_t(a)];
            const Vec2f& ub = uv[face * 3 + size_t(b)];
            const Edge key(mesh.indices[face][a], mesh.indices[face][b],
                           ua.x(), ua.y(), ub.x(), ub.y());
            auto previous = edges.emplace(key, int(face));
            if (!previous.second)
                parent[size_t(root(int(face)))] = root(previous.first->second);
        }
    }
    std::map<int, int> charts;
    std::vector<int> chart_sizes;
    for (size_t face = 0; face < count; ++face) {
        auto item = charts.emplace(root(int(face)), int(charts.size()));
        const int chart = item.first->second;
        if (item.second) {
            layout.chart_centroid.emplace_back(Vec2f::Zero());
            chart_sizes.push_back(0);
        }
        layout.indices.emplace_back(int(face * 3), int(face * 3 + 1), int(face * 3 + 2));
        layout.source_face.push_back(int(face));
        for (int corner = 0; corner < 3; ++corner) {
            layout.source_vertex.push_back(mesh.indices[face][corner]);
            layout.vertex_chart.push_back(chart);
            layout.chart_centroid[size_t(chart)] += uv[face * 3 + size_t(corner)];
            ++chart_sizes[size_t(chart)];
        }
    }
    layout.chart_count = int(charts.size());
    for (size_t chart = 0; chart < charts.size(); ++chart)
        layout.chart_centroid[chart] /= float(chart_sizes[chart]);
    return layout;
}

class IslandCanvas final : public wxPanel
{
public:
    IslandCanvas(wxWindow* parent, PatchUnwrap layout, std::vector<TextureIsland> islands,
                 const TextureDisplacementLayer& layer, bool model_uv, std::vector<Vec2f> base_uvs,
                 const TextureEditSession& session, int slot)
        : wxPanel(parent, wxID_ANY), m_layout(std::move(layout)), m_islands(std::move(islands)),
          m_texture(decode_height_texture(layer)), m_layer(layer), m_normalized_uv(model_uv), m_session(&session), m_slot(slot)
    {
        if (m_islands.size() < size_t(m_layout.chart_count))
            m_islands.resize(size_t(m_layout.chart_count));
        if (!model_uv) m_groups = layer.island_groups;
        m_auto_connect = layer.auto_connect_islands;
        m_base_uvs = std::move(base_uvs);
        if (m_base_uvs.size() != m_layout.uvs.size()) m_base_uvs = m_layout.uvs;
        remember_topology();
        m_vertex_edited.assign(m_layout.uvs.size(),false);
        m_history.push_back(state());
        SetMinSize(wxSize(650, 480));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        fit();
        Bind(wxEVT_PAINT, &IslandCanvas::paint, this);
        Bind(wxEVT_LEFT_DOWN, &IslandCanvas::down, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { commit_history(); if (HasCapture()) ReleaseMouse(); });
        Bind(wxEVT_MOTION, &IslandCanvas::move, this);
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            const Vec2f anchor = uv(event.GetPosition());
            m_span = std::clamp(m_span * (event.GetWheelRotation() > 0 ? .9f : 1.f / .9f), .0001f, 1e6f);
            m_center += anchor - uv(event.GetPosition());
            invalidate_background(); Refresh(false);
        });
        Bind(wxEVT_MIDDLE_DOWN, [this](wxMouseEvent& event) {
            m_pan_point = event.GetPosition(); m_panning = true;
            if (!HasCapture()) CaptureMouse();
        });
        Bind(wxEVT_MIDDLE_UP, [this](wxMouseEvent&) { m_panning = false; if (HasCapture()) ReleaseMouse(); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_panning = false; commit_history(); });
    }
    ~IslandCanvas() override { if (HasCapture()) ReleaseMouse(); }
    void set_mode(int mode)
    {
        m_mode = std::clamp(mode,0,2); m_selected = -1;
        m_vertex_selection.clear(); m_selected_vertices.clear(); m_selected_islands.clear(); m_move_islands.clear(); Refresh(false);
    }
    const std::vector<int>& groups() const { return m_groups; }
    const std::vector<std::pair<int,int>>& seams() const { return m_layer.lscm_seam_edges; }
    void cut_selected()
    {
        if (m_mode != 0 || m_normalized_uv || m_selected < 0)
            throw std::runtime_error("Select an LSCM island first");
        auto cut = cut_texture_island(m_session->source->its,m_session->masks[size_t(m_slot)],m_session->volume_to_world,
                                      m_layer,m_layout,m_islands,m_groups,m_selected);
        m_layout = std::move(cut.layout); m_base_uvs = std::move(cut.base_uvs);
        m_islands = std::move(cut.islands); m_groups = std::move(cut.groups); m_layer.lscm_seam_edges = std::move(cut.seams);
        remember_topology();
        m_vertex_edited.assign(m_layout.uvs.size(),true); m_clear_edits = true;
        clear_selection(); commit_history(); Refresh(false);
    }
    bool auto_connect() const { return m_auto_connect; }
    void set_auto_connect(bool enabled)
    {
        if (m_normalized_uv || !set_texture_islands_connected(m_layout,enabled,m_auto_connect,m_islands)) return;
        commit_history(); Refresh(false);
    }
    void average_scale()
    {
        if (m_normalized_uv || !average_texture_island_scale(m_islands)) return;
        commit_history(); Refresh(false);
    }
    bool join_selected()
    {
        if (m_mode != 0 || m_normalized_uv || !join_nearest_texture_island(m_layout,m_selected,m_islands,m_groups)) return false;
        commit_history(); Refresh(false); return true;
    }
    bool unjoin_selected()
    {
        if (m_mode != 0 || m_normalized_uv || m_selected < 0) return false;
        unjoin_texture_island_group(m_groups,m_selected,m_layout.chart_count);
        m_islands[size_t(m_selected)] = {};
        commit_history(); Refresh(false); return true;
    }
    void select_all_islands()
    {
        if (m_mode != 0) return;
        m_selected_islands.resize(size_t(m_layout.chart_count));
        std::iota(m_selected_islands.begin(),m_selected_islands.end(),0);
        m_selected = m_selected_islands.empty() ? -1 : m_selected_islands.back(); Refresh(false);
    }
    void clear_selection()
    {
        m_selected = -1; m_selected_islands.clear(); m_move_islands.clear(); m_vertex_selection.clear(); m_selected_vertices.clear(); Refresh(false);
    }
    bool clears_uv_edits() const { return m_clear_edits; }
    std::vector<std::pair<int,Vec2f>> vertex_edits() const
    {
        std::vector<std::pair<int,Vec2f>> result;
        for (size_t i=0;i<m_vertex_edited.size();++i)
            if (m_vertex_edited[i]) result.emplace_back(int(i),m_layout.uvs[i]);
        return result;
    }
    void clear_uv_edits()
    {
        m_layout.uvs = m_base_uvs; m_vertex_edited.assign(m_layout.uvs.size(),false); m_clear_edits = true;
        m_selected_vertices = m_vertex_selection.vertices(m_layout);
        commit_history(); Refresh(false);
    }
    void undo()
    {
        if (m_history_cursor==0) return;
        restore(m_history[--m_history_cursor]);
    }
    void redo()
    {
        if (m_history_cursor+1>=m_history.size()) return;
        restore(m_history[++m_history_cursor]);
    }
    bool has_texture() const { return !m_texture.empty(); }
    void show_texture(bool show) { m_show_texture = show; Refresh(false); }
    const std::vector<TextureIsland>& islands() const { return m_islands; }
    std::vector<Vec2f> placed_uvs() const
    {
        std::vector<Vec2f> result;
        result.reserve(m_layout.uvs.size());
        for (size_t i = 0; i < m_layout.uvs.size(); ++i)
            result.push_back(placed(i));
        return result;
    }
    void rotate(float degrees)
    {
        if (m_mode != 0) { transform_vertices(degrees,1.f); return; }
        if (m_selected >= 0) {
            m_islands[size_t(m_selected)].rotation_deg += degrees;
            commit_history(); Refresh(false);
        }
    }
    void scale(float factor)
    {
        if (m_mode != 0) { transform_vertices(0.f,factor); return; }
        if (m_selected >= 0) {
            auto& value = m_islands[size_t(m_selected)].scale;
            value = std::clamp(value * factor, .01f, 100.f);
            commit_history(); Refresh(false);
        }
    }
    void reset()
    {
        m_islands.assign(size_t(m_layout.chart_count), {});
        commit_history(); fit(); Refresh(false);
    }
    void fit()
    {
        Vec2f lo = placed(0), hi = lo;
        for (size_t i = 1; i < m_layout.uvs.size(); ++i) {
            lo = lo.cwiseMin(placed(i)); hi = hi.cwiseMax(placed(i));
        }
        m_center = (lo + hi) * .5f;
        m_span = std::max(.01f, (hi - lo).maxCoeff()) * 1.2f;
        invalidate_background();
    }
private:
    struct State {
        std::vector<Vec2f> uvs;
        std::vector<TextureIsland> islands;
        std::vector<int> groups;
        std::vector<bool> vertex_edited;
        bool clear_edits;
        bool auto_connect;
        std::shared_ptr<const PatchUnwrap> topology;
        std::vector<std::pair<int,int>> seams;
    };
    void remember_topology()
    {
        // Topology is shared by all stroke/placement history entries; only a cut
        // needs another copy. Avoid duplicating indices and maps on every drag.
        auto topology = std::make_shared<PatchUnwrap>(m_layout);
        topology->uvs = m_base_uvs;
        m_topology = std::move(topology);
    }
    State state() const { return {m_layout.uvs,m_islands,m_groups,m_vertex_edited,m_clear_edits,m_auto_connect,m_topology,m_layer.lscm_seam_edges}; }
    void restore(const State& value)
    {
        const bool topology_changed=m_topology!=value.topology;
        m_topology=value.topology; m_layout=*m_topology; m_base_uvs=m_layout.uvs;
        m_layout.uvs=value.uvs; m_islands=value.islands; m_groups=value.groups; m_vertex_edited=value.vertex_edited; m_clear_edits=value.clear_edits;
        m_layer.lscm_seam_edges=value.seams;
        m_auto_connect=value.auto_connect;
        if (topology_changed) clear_selection();
        else {
            m_move_islands.clear();
            m_selected_vertices = m_vertex_selection.vertices(m_layout);
        }
        Refresh(false);
    }
    void commit_history()
    {
        if (m_history.empty()) return;
        const State next=state(); const State& previous=m_history[m_history_cursor];
        bool same=next.uvs==previous.uvs && next.vertex_edited==previous.vertex_edited &&
            next.clear_edits==previous.clear_edits && next.auto_connect==previous.auto_connect &&
            next.topology==previous.topology && next.seams==previous.seams &&
            next.groups==previous.groups && next.islands.size()==previous.islands.size();
        for (size_t i=0;same && i<next.islands.size();++i)
            same=next.islands[i].offset==previous.islands[i].offset &&
                next.islands[i].rotation_deg==previous.islands[i].rotation_deg && next.islands[i].scale==previous.islands[i].scale;
        if (same) return;
        m_history.erase(m_history.begin()+ptrdiff_t(m_history_cursor+1),m_history.end());
        m_history.push_back(next); ++m_history_cursor;
        if (m_history.size()>65) { m_history.erase(m_history.begin()); --m_history_cursor; }
    }
    void transform_vertices(float degrees,float factor)
    {
        if (m_selected_vertices.empty()) return;
        Vec2f center=Vec2f::Zero();
        std::vector<Vec2f> positions;
        for (int vertex:m_selected_vertices) {
            const Vec2f point=placed(size_t(vertex));
            if (std::find(positions.begin(),positions.end(),point)==positions.end()) { positions.push_back(point); center+=point; }
        }
        center/=float(positions.size());
        const float radians=degrees*float(M_PI)/180.f, cs=std::cos(radians), sn=std::sin(radians);
        for (int vertex:m_selected_vertices) {
            const Vec2f old=placed(size_t(vertex)), relative=(old-center)*factor;
            const Vec2f target=center+Vec2f(relative.x()*cs-relative.y()*sn,relative.x()*sn+relative.y()*cs);
            move_texture_uv_vertices(m_layout,m_islands,{vertex},target-old);
            m_vertex_edited[size_t(vertex)]=true;
        }
        commit_history(); Refresh(false);
    }
    Vec2f placed(size_t i) const
    {
        return apply_island_transform(m_layout.uvs[i], m_layout.vertex_chart[i], m_layout, m_islands);
    }
    float zoom() const { return float(std::max(1, std::min(GetClientSize().x, GetClientSize().y))) / m_span; }
    wxPoint screen(const Vec2f& uv) const
    {
        const auto size = GetClientSize();
        return {int(size.x * .5f + (uv.x() - m_center.x()) * zoom()),
                int(size.y * .5f - (uv.y() - m_center.y()) * zoom())};
    }
    Vec2f uv(const wxPoint& p) const
    {
        const auto size = GetClientSize();
        return m_center + Vec2f(p.x - size.x * .5f, size.y * .5f - p.y) / zoom();
    }
    void down(wxMouseEvent& event)
    {
        m_last = uv(event.GetPosition()); m_move_islands.clear();
        if (m_mode != 0) {
            m_selected = -1;
            const Vec2f mouse(float(event.GetX()),float(event.GetY()));
            float best=100.f; int first=-1,second=-1;
            if (m_mode==1) {
                for (size_t i=0;i<m_layout.uvs.size();++i) {
                    const auto pixel=screen(placed(i));
                    const float distance=(Vec2f(float(pixel.x),float(pixel.y))-mouse).squaredNorm();
                    if (distance<best) { best=distance; first=int(i); }
                }
            } else {
                for (const auto& face:m_layout.indices)
                    for (int corner=0;corner<3;++corner) {
                        const int a=face[corner],b=face[(corner+1)%3];
                        const auto pa=screen(placed(size_t(a))),pb=screen(placed(size_t(b)));
                        const Vec2f start(float(pa.x),float(pa.y)),edge(float(pb.x-pa.x),float(pb.y-pa.y));
                        const float length=edge.squaredNorm();
                        const Vec2f nearest=start+edge*(length>1e-12f ? std::clamp((mouse-start).dot(edge)/length,0.f,1.f) : 0.f);
                        const float distance=(nearest-mouse).squaredNorm();
                        if (distance<best) { best=distance; first=a; second=b; }
                    }
            }
            const bool active = m_vertex_selection.select(m_layout,first,second,event.ShiftDown(),
                                                          event.ControlDown() || event.CmdDown());
            m_selected_vertices = m_vertex_selection.vertices(m_layout);
            if (active) {
                m_selected=m_layout.vertex_chart[size_t(first)];
                if (!HasCapture()) CaptureMouse();
            }
            Refresh(false); return;
        }
        const auto cross = [](const Vec2f& a, const Vec2f& b) { return a.x() * b.y() - a.y() * b.x(); };
        int hit = -1;
        for (auto face = m_layout.indices.rbegin(); face != m_layout.indices.rend(); ++face) {
            const Vec2f a = placed(size_t((*face)[0])), b = placed(size_t((*face)[1])), c = placed(size_t((*face)[2]));
            if (std::abs(cross(b - a, c - a)) < 1e-12f) continue;
            const float ab = cross(b - a, m_last - a), bc = cross(c - b, m_last - b), ca = cross(a - c, m_last - c);
            if ((ab >= 0 && bc >= 0 && ca >= 0) || (ab <= 0 && bc <= 0 && ca <= 0)) {
                hit = m_layout.vertex_chart[size_t((*face)[0])];
                break;
            }
        }
        if (select_texture_island(m_selected_islands,m_selected,hit,event.ShiftDown(),event.ControlDown() || event.CmdDown())) {
            m_move_islands = texture_island_move_set(m_groups,m_layout.chart_count,m_selected_islands,m_selected);
            if (!HasCapture()) CaptureMouse();
        }
        Refresh(false);
    }
    void move(wxMouseEvent& event)
    {
        if (m_panning && event.MiddleIsDown()) {
            m_center += uv(m_pan_point) - uv(event.GetPosition());
            m_pan_point = event.GetPosition(); invalidate_background(); Refresh(false); return;
        }
        if (!event.Dragging() || !event.LeftIsDown() || m_selected < 0)
            return;
        const Vec2f current = uv(event.GetPosition());
        if (m_mode==0) {
            for (const int chart : m_move_islands) m_islands[size_t(chart)].offset += current-m_last;
        } else {
            move_texture_uv_vertices(m_layout,m_islands,m_selected_vertices,current-m_last);
            for (int vertex:m_selected_vertices) m_vertex_edited[size_t(vertex)]=true;
        }
        m_last = current;
        Refresh(false);
    }
    void invalidate_background() { m_background = wxBitmap(); }
    void rebuild_background()
    {
        const auto size = GetClientSize();
        if (m_texture.empty() || size.x <= 0 || size.y <= 0) return;
        if (m_background.IsOk() && m_background_size == size) return;
        // The backdrop is sampled in the same final UV space as Bake. Keep it
        // cached while islands move; only the viewport changes this image.
        const int width = std::min(1024, size.x), height = std::min(1024, size.y);
        wxImage image(width, height);
        auto* pixels = image.GetData();
        const float aspect = float(m_texture.width) / float(m_texture.height);
        const Vec2f origin = apply_uv_transform(Vec2f::Zero(), m_layer, aspect, m_normalized_uv);
        const Vec2f dx = apply_uv_transform(Vec2f::UnitX(), m_layer, aspect, m_normalized_uv) - origin;
        const Vec2f dy = apply_uv_transform(Vec2f::UnitY(), m_layer, aspect, m_normalized_uv) - origin;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const Vec2f planar = m_center + Vec2f((float(x) + .5f) * size.x / width - size.x * .5f,
                    size.y * .5f - (float(y) + .5f) * size.y / height) / zoom();
                const Vec2f sample_uv = origin + dx * planar.x() + dy * planar.y();
                DecodedHeightTexture::TexelTap tap;
                Vec3f color(.14f, .15f, .17f);
                if (m_texture.texel_tap(sample_uv, m_layer.tile_enabled, m_layer.tile_method, tap))
                    color = m_texture.has_color() ? m_texture.sample_color(sample_uv, m_layer.tile_enabled, m_layer.tile_method) :
                        Vec3f::Constant(m_texture.sample(sample_uv, m_layer.tile_enabled, m_layer.tile_method));
                const size_t pixel = (size_t(y) * size_t(width) + size_t(x)) * 3;
                for (int channel = 0; channel < 3; ++channel)
                    pixels[pixel + size_t(channel)] = static_cast<unsigned char>(std::clamp(color[channel] * 255.f, 0.f, 255.f));
            }
        if (width != size.x || height != size.y) image.Rescale(size.x, size.y, wxIMAGE_QUALITY_BILINEAR);
        m_background = wxBitmap(image); m_background_size = size;
    }
    void paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(wxColour(36, 39, 43))); dc.Clear();
        const bool backdrop = m_show_texture && !m_texture.empty();
        if (backdrop) {
            rebuild_background();
            if (m_background.IsOk()) dc.DrawBitmap(m_background, 0, 0, false);
        } else {
            dc.SetPen(wxPen(wxColour(69, 73, 78)));
            const auto size = GetClientSize();
            for (int x = 0; x < size.x; x += 30) dc.DrawLine(x, 0, x, size.y);
            for (int y = 0; y < size.y; y += 30) dc.DrawLine(0, y, size.x, y);
        }
        for (const auto& face : m_layout.indices) {
            const int chart = m_layout.vertex_chart[size_t(face[0])];
            const bool active = m_mode == 0 ? std::find(m_selected_islands.begin(),m_selected_islands.end(),chart) != m_selected_islands.end() : chart == m_selected;
            dc.SetBrush(backdrop ? *wxTRANSPARENT_BRUSH : wxBrush(active ? wxColour(195, 145, 55) : wxColour(56, 107, 132)));
            dc.SetPen(wxPen(active ? wxColour(255, 223, 126) : wxColour(40, 190, 255), active ? 2 : 1));
            wxPoint points[3]{screen(placed(size_t(face[0]))), screen(placed(size_t(face[1]))), screen(placed(size_t(face[2])))};
            dc.DrawPolygon(3, points);
        }
        if (m_mode==2) {
            dc.SetPen(wxPen(wxColour(255,100,45),3));
            for (const auto& edge : m_vertex_selection.primitives())
                dc.DrawLine(screen(placed(size_t(edge.first))),screen(placed(size_t(edge.second))));
        }
        if (m_mode!=0) {
            dc.SetPen(wxPen(wxColour(20,20,20)));
            for (size_t i=0;i<m_layout.uvs.size();++i) {
                const bool selected=std::find(m_selected_vertices.begin(),m_selected_vertices.end(),int(i))!=m_selected_vertices.end();
                dc.SetBrush(wxBrush(selected ? wxColour(255,100,45) : wxColour(235,235,235)));
                dc.DrawCircle(screen(placed(i)),selected ? 4 : 2);
            }
        }
    }
    std::vector<Vec2f> m_base_uvs;
    std::vector<bool> m_vertex_edited;
    TextureUVSelection m_vertex_selection;
    std::vector<int> m_selected_vertices;
    std::vector<State> m_history;
    size_t m_history_cursor=0;
    bool m_clear_edits=false;
    int m_mode=0;
    DecodedHeightTexture m_texture;
    TextureDisplacementLayer m_layer;
    bool m_normalized_uv = false, m_show_texture = true, m_panning = false;
    bool m_auto_connect = false;
    const TextureEditSession* m_session;
    int m_slot;
    std::shared_ptr<const PatchUnwrap> m_topology;
    wxBitmap m_background;
    wxSize m_background_size;
    wxPoint m_pan_point;
    PatchUnwrap m_layout;
    std::vector<TextureIsland> m_islands;
    std::vector<int> m_groups, m_selected_islands, m_move_islands;
    Vec2f m_center = Vec2f::Zero(), m_last = Vec2f::Zero();
    float m_span = 1.f;
    int m_selected = -1;
};
} // namespace

bool edit_texture_uv(wxWindow* parent, TextureEditSession& session, int slot)
{
    auto* original = session.layer(slot);
    if (!original)
        return false;
    const bool model_uv = original->projection_method == TextureProjectionMethod::ModelUV;
    if (model_uv && session.model_uv.empty())
        throw std::runtime_error("This model has no imported UV channel");
    TextureDisplacementLayer edited = *original;
    PatchUnwrap unwrap;
    std::vector<TextureIsland> islands;
    std::vector<Vec2f> base_uvs;
    if (model_uv) {
        unwrap = model_uv_layout(session.source->its, session.model_uv);
        base_uvs=unwrap.uvs;
    } else {
        auto patch_seams = edited.lscm_seam_edges; // Resolve a working copy; persisted endpoints stay in the original mesh.
        auto patch = extract_painted_patch(session.source->its, session.masks[size_t(slot)], patch_seams);
        if (patch.indices.empty())
            throw std::runtime_error("Paint an area of this layer before editing its UV islands");
        const Transform3d frame = texture_displacement_bake_frame(session.volume_to_world);
        for (auto& v : patch.vertices) v = (frame * v.cast<double>()).cast<float>();
        // Match the fixed upstream editor and the bake sampler: both use zero
        // chart padding. A different packing changes the coordinates of later islands.
        unwrap = compute_patch_unwrap(patch, edited.lscm_seam_angle_deg, 0.f, patch_seams);
        base_uvs=unwrap.uvs;
        apply_lscm_uv_overrides(unwrap, edited.lscm_uv_overrides);
        islands = edited.islands;
        if (edited.auto_connect_islands && islands.empty())
            islands = compute_connected_net(unwrap);
    }
    if (unwrap.empty() || unwrap.uvs.empty())
        throw std::runtime_error("The painted surface could not be unwrapped");
    wxDialog dialog(parent, wxID_ANY, _L("UV islands"), wxDefaultPosition, wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* edit_help = new wxStaticText(&dialog, wxID_ANY, _L("Choose Island, Vertex or Edge, then drag to move. Shift adds and Ctrl/Cmd toggles selection in every mode; dragging a selected element moves the selection. Joined islands move together. In Island mode, rotate/scale acts on the last selected island. Scroll zooms; middle drag pans. Cancel preserves the previous layout."));
    edit_help->Wrap(760); layout->Add(edit_help, 0, wxALL, 10);
    auto* canvas = new IslandCanvas(&dialog, std::move(unwrap), std::move(islands), edited, model_uv, std::move(base_uvs),session,slot);
    layout->Add(canvas, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);
    auto* select_mode=new wxChoice(&dialog,wxID_ANY);
    select_mode->Append(_L("Island")); select_mode->Append(_L("Vertex")); select_mode->Append(_L("Edge"));
    select_mode->SetSelection(0);
    select_mode->Bind(wxEVT_CHOICE,[canvas,select_mode](wxCommandEvent&) { canvas->set_mode(select_mode->GetSelection()); });
    layout->Add(select_mode,0,wxLEFT|wxRIGHT|wxTOP,10);
    auto* show_image = new wxCheckBox(&dialog, wxID_ANY, _L("Show texture image"));
    show_image->SetValue(canvas->has_texture()); show_image->Enable(canvas->has_texture());
    show_image->Bind(wxEVT_CHECKBOX, [canvas, show_image](wxCommandEvent&) { canvas->show_texture(show_image->GetValue()); });
    layout->Add(show_image, 0, wxALL, 10);
    auto* commands = new wxBoxSizer(wxHORIZONTAL);
    const auto button = [&](const wxString& label, std::function<void()> action) {
        auto* control = new wxButton(&dialog, wxID_ANY, label);
        control->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
        commands->Add(control, 0, wxRIGHT, 6);
    };
    button(_L("Rotate left"), [canvas] { canvas->rotate(-15.f); });
    button(_L("Rotate right"), [canvas] { canvas->rotate(15.f); });
    button(_L("Scale down"), [canvas] { canvas->scale(1.f / 1.1f); });
    button(_L("Scale up"), [canvas] { canvas->scale(1.1f); });
    button(_L("Reset layout"), [canvas] { canvas->reset(); });
    button(_L("Fit"), [canvas] { canvas->fit(); canvas->Refresh(false); });
    layout->Add(commands, 0, wxALL, 10);
    auto* group_commands = new wxBoxSizer(wxHORIZONTAL);
    const auto group_button = [&](const wxString& label, std::function<void()> action) {
        auto* control = new wxButton(&dialog,wxID_ANY,label);
        control->Bind(wxEVT_BUTTON,[action](wxCommandEvent&) { action(); });
        group_commands->Add(control,0,wxRIGHT,6); return control;
    };
    group_button(_L("Select all islands"),[canvas] { canvas->select_all_islands(); });
    group_button(_L("Clear selection"),[canvas] { canvas->clear_selection(); });
    auto* join = group_button(_L("Join nearest"),[canvas,&dialog] {
        if (!canvas->join_selected()) wxMessageBox(_L("Select an LSCM island that shares a model edge with another island."),_L("Join UV islands"),wxOK|wxICON_INFORMATION,&dialog);
    });
    join->Enable(!model_uv);
    join->SetToolTip(_L("Align the selected island with its nearest edge-sharing neighbor and group them for moving."));
    auto* unjoin = group_button(_L("Unjoin island"),[canvas,&dialog] {
        if (!canvas->unjoin_selected()) wxMessageBox(_L("Select an LSCM island first."),_L("Unjoin UV island"),wxOK|wxICON_INFORMATION,&dialog);
    });
    unjoin->Enable(!model_uv);
    unjoin->SetToolTip(_L("Detach the selected island from its group and reset its placement."));
    layout->Add(group_commands,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
    auto* layout_commands = new wxBoxSizer(wxHORIZONTAL);
    auto* cut = new wxButton(&dialog,wxID_ANY,_L("Cut island"));
    cut->Enable(!model_uv);
    cut->SetToolTip(_L("Cut the selected island across its longest model axis using original mesh edges. Existing texture placement is preserved."));
    cut->Bind(wxEVT_BUTTON,[canvas,&dialog](wxCommandEvent&) {
        try { canvas->cut_selected(); }
        catch (const std::exception& e) { wxMessageBox(wxString::FromUTF8(e.what()),_L("Cut UV island"),wxOK|wxICON_INFORMATION,&dialog); }
    });
    layout_commands->Add(cut,0,wxRIGHT,6);
    auto* average = new wxButton(&dialog,wxID_ANY,_L("Average island scale"));
    average->Enable(!model_uv);
    average->SetToolTip(_L("Set all LSCM islands to their mean scale, preserving their position and rotation."));
    average->Bind(wxEVT_BUTTON,[canvas](wxCommandEvent&) { canvas->average_scale(); });
    layout_commands->Add(average,0,wxRIGHT,12);
    auto* auto_connect = new wxCheckBox(&dialog,wxID_ANY,_L("Auto-connect islands"));
    auto_connect->SetValue(canvas->auto_connect()); auto_connect->Enable(!model_uv);
    auto_connect->SetToolTip(_L("Enabling unfolds adjacent LSCM islands into connected nets. Disabling preserves the current layout."));
    auto_connect->Bind(wxEVT_CHECKBOX,[canvas,auto_connect](wxCommandEvent&) { canvas->set_auto_connect(auto_connect->GetValue()); });
    layout_commands->Add(auto_connect,0,wxALIGN_CENTER_VERTICAL);
    layout->Add(layout_commands,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
    auto* edits=new wxBoxSizer(wxHORIZONTAL);
    const auto edit_button=[&](const wxString& label,std::function<void()> action) {
        auto* control=new wxButton(&dialog,wxID_ANY,label);
        control->Bind(wxEVT_BUTTON,[action](wxCommandEvent&) { action(); });
        edits->Add(control,0,wxRIGHT,6); return control;
    };
    edit_button(_L("Undo UV edit"),[canvas,auto_connect] { canvas->undo(); auto_connect->SetValue(canvas->auto_connect()); });
    edit_button(_L("Redo UV edit"),[canvas,auto_connect] { canvas->redo(); auto_connect->SetValue(canvas->auto_connect()); });
    edit_button(_L("Clear vertex edits"),[canvas] { canvas->clear_uv_edits(); })->Enable(!model_uv);
    layout->Add(edits,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
    layout->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxALIGN_RIGHT, 10);
    dialog.SetSizerAndFit(layout); dialog.CentreOnParent();
    if (dialog.ShowModal() != wxID_OK)
        return false;
    if (model_uv)
        session.model_uv = canvas->placed_uvs();
    else {
        edited.projection_method = TextureProjectionMethod::LSCM;
        edited.islands = canvas->islands();
        edited.island_groups = canvas->groups();
        edited.lscm_seam_edges = canvas->seams();
        edited.auto_connect_islands = canvas->auto_connect();
        if (canvas->clears_uv_edits()) edited.lscm_uv_overrides.clear();
        for (const auto& [vertex,raw_uv]:canvas->vertex_edits()) set_texture_uv_override(edited,vertex,raw_uv);
        *session.layer(slot) = std::move(edited);
    }
    return true;
}
} // namespace Slic3r::GUI
