// Native preview and TriangleSelector painting, adapted from the phase-1 GL preview.
class EditableTexturePreview final : public wxPanel
{
public:
    EditableTexturePreview(wxWindow* parent, std::shared_ptr<const indexed_triangle_set> mesh)
        : wxPanel(parent), m_mesh(std::move(mesh))
    {
        m_canvas = OpenGLManager::create_wxglcanvas(*this);
        m_context = wxGetApp().init_glcontext(*m_canvas);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(m_canvas, 1, wxEXPAND);
        SetSizer(layout);
        SetMinSize(wxSize(480, 420));
        Vec3f lo = m_mesh->vertices.front(), hi = lo;
        for (const auto& p : m_mesh->vertices) { lo = lo.cwiseMin(p); hi = hi.cwiseMax(p); }
        m_center = ((lo + hi) * .5f).cast<double>();
        m_radius = std::max(0.01, double((hi - lo).norm()) * .65);
        m_canvas->Bind(wxEVT_PAINT, &EditableTexturePreview::paint, this);
        m_canvas->Bind(wxEVT_SIZE, &EditableTexturePreview::resize, this);
        m_canvas->Bind(wxEVT_LEFT_DOWN, &EditableTexturePreview::mouse_down, this);
        m_canvas->Bind(wxEVT_LEFT_UP, &EditableTexturePreview::mouse_up, this);
        m_canvas->Bind(wxEVT_MOTION, &EditableTexturePreview::mouse_move, this);
        m_canvas->Bind(wxEVT_MOUSEWHEEL, &EditableTexturePreview::wheel, this);
        m_canvas->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { finish_stroke(); });
    }

    ~EditableTexturePreview() override
    {
        m_stroke = {}; // Children are destroyed after the owning dialog members.
        m_stroking = false;
        // Both replacement and destruction release VBO/IBO/VAO while this canvas's context is current.
        if (m_canvas && m_context) {
            m_canvas->SetCurrent(*m_context);
            m_model.reset();
            m_colored_models.clear();
            m_overlay.reset();
            m_seam_model.reset(); m_hover_model.reset(); m_anchor_model.reset();
            if (m_canvas->HasCapture())
                m_canvas->ReleaseMouse();
        }
    }

    void configure_paint(std::shared_ptr<const TriangleMesh> source, const Transform3d& to_world,
                         std::function<void(const TriangleSelector::TriangleSplittingData&)> changed,
                         std::function<void(bool)> stroke = {})
    {
        m_source = std::move(source);
        m_to_world = to_world;
        m_display_frame = texture_displacement_bake_frame(to_world);
        Vec3d lo = m_display_frame * m_source->its.vertices.front().cast<double>(), hi = lo;
        for (const auto& point : m_source->its.vertices) {
            const Vec3d display = m_display_frame * point.cast<double>();
            lo = lo.cwiseMin(display); hi = hi.cwiseMax(display);
        }
        m_center = (lo + hi) * .5;
        m_radius = std::max(.01, (hi - lo).norm() * .65);
        m_raycaster = std::make_unique<AABBMesh>(*m_source);
        m_selector = std::make_unique<TriangleSelector>(*m_source);
        m_changed = std::move(changed); m_stroke = std::move(stroke);
    }
    void finish_stroke()
    {
        m_previous_hit.reset();
        if (m_stroking) {
            m_stroking = false;
            if (m_stroke) m_stroke(false);
        }
    }
    void set_mask(const TriangleSelector::TriangleSplittingData& mask)
    {
        m_selector->deserialize(mask);
        m_overlay_dirty = true;
        m_canvas->Refresh(false);
    }
    void set_tool(int tool, float radius, bool can_paint, float fill_angle = 30.f)
    {
        if (m_tool != tool) { m_seam_anchor = -1; m_seam_hover = {-1,-1}; m_seams_dirty = true; }
        m_tool = tool; m_brush_radius = radius; m_can_paint = can_paint; m_fill_angle = fill_angle;
    }
    void set_seams(std::vector<std::pair<int,int>> seams, int slot,
                   std::function<void(const std::vector<std::pair<int,int>>&)> changed)
    {
        if (m_seam_slot != slot || seams != m_seams) {
            m_seam_anchor = -1; m_seam_hover = {-1,-1}; m_seams_dirty = true;
        }
        m_seams = std::move(seams); m_seam_slot = slot; m_seam_changed = std::move(changed);
    }
    void project_from_view(TextureDisplacementLayer& layer) const
    {
        const Matrix3d rotation = (Eigen::AngleAxisd(m_pitch, Vec3d::UnitX()) *
                                   Eigen::AngleAxisd(m_yaw, Vec3d::UnitZ())).toRotationMatrix();
        const Vec3d right = rotation.transpose().col(0);
        const Vec3d up = rotation.transpose().col(1);
        layer.view_project_right = right.cast<float>();
        layer.view_project_up = up.cast<float>();
        layer.view_project_projective = false;
        layer.projection_method = TextureProjectionMethod::ViewProjected;
    }

    wxRect default_projection_frame() const
    {
        const wxRect viewport=m_canvas->GetScreenRect();
        const wxSize size(std::max(80,viewport.width/2),std::max(80,viewport.height/2));
        return wxRect(viewport.GetTopLeft()+wxPoint((viewport.width-size.x)/2,(viewport.height-size.y)/2),size);
    }
    void project_frame(TextureDisplacementLayer& layer, const wxRect& screen_gate) const
    {
        const wxSize size=m_canvas->GetClientSize();
        const double retina=m_canvas->GetContentScaleFactor();
        const wxPoint top_left=m_canvas->ScreenToClient(screen_gate.GetTopLeft());
        const int width=std::max(1,int(size.x*retina)), height=std::max(1,int(size.y*retina));
        const double aspect=double(width)/height;
        Transform3d view=Transform3d::Identity();
        view.linear()=(Eigen::AngleAxisd(m_pitch,Vec3d::UnitX())*Eigen::AngleAxisd(m_yaw,Vec3d::UnitZ())).toRotationMatrix();
        view.translation()=-view.linear()*m_center;
        Eigen::Matrix4d projection=Eigen::Matrix4d::Identity();
        projection(0,0)=m_zoom/(m_radius*std::max(1.,aspect));
        projection(1,1)=m_zoom/(m_radius*std::max(1.,1./aspect));
        projection(2,2)=-1./(m_radius*4.);
        const auto matrix=texture_frame_projection(projection*view.matrix(),Vec2d(width,height),
            Eigen::Vector4d(top_left.x*retina,top_left.y*retina,screen_gate.width*retina,screen_gate.height*retina));
        project_from_view(layer);
        layer.view_project_projective=true; layer.view_project_matrix=matrix; layer.tile_enabled=false;
    }
    TriangleSelector::TriangleSplittingData visible_mask(const std::array<float,12>* frame_clip = nullptr) const
    {
        if (!m_source || !m_raycaster) throw std::runtime_error("Texture source is unavailable");
        const Matrix3d rotation = (Eigen::AngleAxisd(m_pitch, Vec3d::UnitX()) *
                                   Eigen::AngleAxisd(m_yaw, Vec3d::UnitZ())).toRotationMatrix();
        return texture_visible_mask(*m_source, texture_visible_faces(*m_source, *m_raycaster,
            m_display_frame, rotation.transpose() * -Vec3d::UnitZ(),frame_clip));
    }

    void set_default_filament(int filament) { m_default_filament = filament; }
    void set_mesh(std::shared_ptr<const indexed_triangle_set> mesh, std::vector<uint8_t> filaments = {},
                  TexturePalette palette = {})
    {
        m_mesh = std::move(mesh);
        m_filaments = std::move(filaments);
        m_palette = std::move(palette);
        m_dirty = true;
        m_canvas->Refresh(false);
    }

private:
    void resize(wxSizeEvent& event) { m_canvas->Refresh(false); event.Skip(); }
    void mouse_down(wxMouseEvent& event)
    {
        m_last = event.GetPosition();
        m_previous_hit.reset();
        if (m_tool >= 5 && m_can_paint) { seam_click(event); return; }
        if (m_tool != 0 && m_can_paint) {
            m_stroking = true;
            if (m_stroke) m_stroke(true);
            paint_mask(event);
        }
        if (!m_canvas->HasCapture())
            m_canvas->CaptureMouse();
    }
    void mouse_up(wxMouseEvent&)
    {
        finish_stroke();
        if (m_canvas->HasCapture())
            m_canvas->ReleaseMouse();
    }
    void mouse_move(wxMouseEvent& event)
    {
        if (m_tool >= 5 && m_can_paint) {
            const auto pick = seam_pick(event);
            if (pick != m_seam_hover) { m_seam_hover = pick; m_seams_dirty = true; m_canvas->Refresh(false); }
            return;
        }
        if (!event.Dragging() || !event.LeftIsDown())
            return;
        if (m_tool != 0 && m_can_paint) { if (m_stroking) paint_mask(event); return; }
        const auto p = event.GetPosition();
        m_yaw += double(p.x - m_last.x) * .01;
        m_pitch = std::clamp(m_pitch + double(p.y - m_last.y) * .01, -1.5, 1.5);
        m_last = p;
        m_canvas->Refresh(false);
    }
    void wheel(wxMouseEvent& event)
    {
        m_zoom = std::clamp(m_zoom * (event.GetWheelRotation() > 0 ? 1.1 : 1. / 1.1), .2, 5.);
        m_canvas->Refresh(false);
    }
    std::pair<Vec3d,Vec3d> mouse_ray(const wxMouseEvent& event) const
    {
        const wxSize size = m_canvas->GetClientSize();
        const double width = std::max(1, size.x), height = std::max(1, size.y);
        const double aspect = width / height;
        const Matrix3d rotation = (Eigen::AngleAxisd(m_pitch, Vec3d::UnitX()) *
                                   Eigen::AngleAxisd(m_yaw, Vec3d::UnitZ())).toRotationMatrix();
        const Vec3d screen((2. * event.GetX() / width - 1.) * m_radius * std::max(1., aspect) / m_zoom,
                           (1. - 2. * event.GetY() / height) * m_radius * std::max(1., 1. / aspect) / m_zoom,
                           4. * m_radius);
        const Transform3d to_local = m_display_frame.inverse();
        const Vec3d origin = to_local * (m_center + rotation.transpose() * screen);
        const Vec3d direction = (to_local.linear() * rotation.transpose() * -Vec3d::UnitZ()).normalized();
        return {origin,direction};
    }
    std::pair<int,int> seam_pick(const wxMouseEvent& event) const
    {
        if (!m_source || !m_raycaster || m_mesh.get() != &m_source->its) return {-1,-1};
        const auto ray = mouse_ray(event);
        const auto hit = m_raycaster->query_ray_hit(ray.first,ray.second);
        if (!hit.is_hit() || size_t(hit.face()) >= m_source->its.indices.size()) return {-1,-1};
        const auto& mesh = m_source->its;
        const auto& face = mesh.indices[size_t(hit.face())];
        const Vec3f point = hit.position().cast<float>();
        float best_distance = std::numeric_limits<float>::infinity();
        int best = 0;
        for (int corner = 0; corner < 3; ++corner) {
            const Vec3f& a = mesh.vertices[size_t(face[corner])];
            Vec3f nearest = a;
            if (m_tool != 6) {
                const Vec3f edge = mesh.vertices[size_t(face[(corner + 1) % 3])] - a;
                const float length = edge.squaredNorm();
                if (length > 1e-12f) nearest += edge * std::clamp((point-a).dot(edge) / length,0.f,1.f);
            }
            const float distance = (point-nearest).squaredNorm();
            if (distance < best_distance) { best_distance = distance; best = corner; }
        }
        const int a = face[best], b = m_tool == 6 ? a : face[(best + 1) % 3];
        return {std::min(a,b),std::max(a,b)};
    }
    void seam_click(const wxMouseEvent& event)
    {
        const auto pick = seam_pick(event);
        if (pick.first < 0) return;
        bool changed = false;
        if (m_tool == 6) {
            if (m_seam_anchor >= 0 && m_seam_anchor != pick.first) {
                for (const auto& edge : texture_seam_path(m_source->its,m_seam_anchor,pick.first))
                    if (std::find(m_seams.begin(),m_seams.end(),edge) == m_seams.end()) {
                        m_seams.push_back(edge); changed = true;
                    }
            }
            m_seam_anchor = pick.first;
        } else {
            const auto found = std::find(m_seams.begin(),m_seams.end(),pick);
            if (found == m_seams.end()) m_seams.push_back(pick);
            else m_seams.erase(found);
            changed = true;
        }
        if (changed && m_seam_changed) m_seam_changed(m_seams);
        m_seams_dirty = true; m_canvas->Refresh(false);
    }
    void rebuild_seam_models()
    {
        const auto& mesh = m_source->its;
        const auto build = [&mesh](GLModel& model, const std::vector<std::pair<int,int>>& edges, const ColorRGBA& color) {
            model.reset();
            GLModel::Geometry geometry;
            geometry.format = {GLModel::Geometry::EPrimitiveType::Lines,GLModel::Geometry::EVertexLayout::P3};
            for (const auto& [a,b] : edges) {
                if (a < 0 || b < 0 || size_t(a) >= mesh.vertices.size() || size_t(b) >= mesh.vertices.size()) continue;
                const unsigned first = unsigned(geometry.vertices_count());
                geometry.add_vertex(mesh.vertices[size_t(a)]); geometry.add_vertex(mesh.vertices[size_t(b)]);
                geometry.add_line(first,first+1);
            }
            if (!geometry.is_empty()) { model.init_from(std::move(geometry)); model.set_color(color); }
        };
        const auto incident = [&mesh](int vertex) {
            std::vector<std::pair<int,int>> edges;
            if (vertex < 0) return edges;
            for (const auto& face : mesh.indices)
                for (int corner = 0; corner < 3; ++corner) {
                    const int a=face[corner],b=face[(corner+1)%3];
                    if (a==vertex || b==vertex) edges.emplace_back(a,b);
                }
            return edges;
        };
        build(m_seam_model,m_seams,ColorRGBA(1.f,.15f,.15f,1.f));
        build(m_hover_model,m_tool==6 ? incident(m_seam_hover.first) : std::vector<std::pair<int,int>>{m_seam_hover},
              ColorRGBA(1.f,.9f,.15f,1.f));
        build(m_anchor_model,incident(m_seam_anchor),ColorRGBA(.2f,1.f,.4f,1.f));
        m_seams_dirty = false;
    }
    void paint_mask(const wxMouseEvent& event)
    {
        if (!m_selector || !m_raycaster || m_mesh.get() != &m_source->its)
            return;
        const auto ray = mouse_ray(event);
        const Vec3d& origin = ray.first;
        const Vec3d& direction = ray.second;
        const auto hit = m_raycaster->query_ray_hit(origin, direction);
        if (!hit.is_hit()) { m_previous_hit.reset(); return; }
        const auto state = event.ShiftDown() ? EnforcerBlockerType::NONE : EnforcerBlockerType::ENFORCER;
        Transform3d no_translation = m_to_world;
        no_translation.translation().setZero();
        if (m_tool == 2) {
            m_selector->set_facet(hit.face(), state);
        } else if (m_tool == 3) {
            m_selector->seed_fill_select_triangles(hit.position().cast<float>(), hit.face(), no_translation, {}, m_fill_angle, 0.f, true);
            m_selector->seed_fill_apply_on_triangles(state);
        } else {
            const Vec3f point = hit.position().cast<float>();
            const auto shape = m_tool == 4 ? TriangleSelector::CIRCLE : TriangleSelector::SPHERE;
            auto cursor = m_previous_hit && m_previous_state == state ?
                TriangleSelector::DoublePointCursor::cursor_factory(*m_previous_hit, point, origin.cast<float>(),
                    m_brush_radius, shape, m_to_world, {}) :
                TriangleSelector::SinglePointCursor::cursor_factory(point, origin.cast<float>(),
                    m_brush_radius, shape, m_to_world, {});
            m_selector->select_patch(hit.face(), std::move(cursor), state, no_translation, true);
            m_previous_hit = point; m_previous_state = state;
        }
        m_overlay_dirty = true;
        m_changed(m_selector->serialize());
        m_canvas->Refresh(false);
    }
    void paint(wxPaintEvent&)
    {
        wxPaintDC dc(m_canvas);
        if (!m_canvas->IsShownOnScreen() || !m_context || !m_canvas->SetCurrent(*m_context) || !wxGetApp().init_opengl())
            return;
        const wxSize size = m_canvas->GetClientSize();
        const double pixel_scale = m_canvas->GetContentScaleFactor();
        const int width = std::max(1, int(size.x * pixel_scale)), height = std::max(1, int(size.y * pixel_scale));
        glViewport(0, 0, width, height);
        glClearColor(.12f, .13f, .15f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        if (m_dirty) {
            m_model.reset();
            m_colored_models.clear();
            if (m_filaments.size() == m_mesh->indices.size() && !m_palette.empty()) {
                std::map<int, GLModel::Geometry> groups;
                for (size_t face = 0; face < m_mesh->indices.size(); ++face) {
                    const int filament = m_filaments[face] ? m_filaments[face] : m_default_filament;
                    auto& geometry = groups[filament];
                    const auto& triangle = m_mesh->indices[face];
                    const Vec3f& a = m_mesh->vertices[size_t(triangle[0])];
                    const Vec3f& b = m_mesh->vertices[size_t(triangle[1])];
                    const Vec3f& c = m_mesh->vertices[size_t(triangle[2])];
                    Vec3f normal = (b - a).cross(c - a);
                    normal = normal.norm() > 1e-12f ? Vec3f(normal.normalized()) : Vec3f::UnitZ();
                    const unsigned first = unsigned(geometry.vertices_count());
                    geometry.add_vertex(a, normal); geometry.add_vertex(b, normal); geometry.add_vertex(c, normal);
                    geometry.add_triangle(first, first + 1, first + 2);
                }
                for (auto& group : groups) {
                    auto model = std::make_unique<GLModel>();
                    model->init_from(std::move(group.second));
                    ColorRGBA color(.3f, .72f, .92f, 1.f);
                    for (const auto& entry : m_palette)
                        if (entry.filament_id == group.first) { color = ColorRGBA(entry.rgb.x(), entry.rgb.y(), entry.rgb.z(), 1.f); break; }
                    model->set_color(color); m_colored_models.push_back(std::move(model));
                }
            } else {
                m_model.init_from(*m_mesh);
                m_model.set_color(ColorRGBA(.3f, .72f, .92f, 1.f));
            }
            m_dirty = false;
        }
        bool rendered = false;
        if (auto* shader = wxGetApp().get_shader("gouraud_light")) {
            Transform3d view = Transform3d::Identity();
            view.linear() = (Eigen::AngleAxisd(m_pitch, Vec3d::UnitX()) * Eigen::AngleAxisd(m_yaw, Vec3d::UnitZ())).toRotationMatrix();
            view.translation() = -view.linear() * m_center;
            Transform3d projection = Transform3d::Identity();
            const double aspect = double(width) / double(height);
            projection.matrix()(0, 0) = m_zoom / (m_radius * std::max(1., aspect));
            projection.matrix()(1, 1) = m_zoom / (m_radius * std::max(1., 1. / aspect));
            projection.matrix()(2, 2) = -1. / (m_radius * 4.);
            shader->start_using();
            shader->set_uniform("emission_factor", .05f);
            const Transform3d view_model = view * m_display_frame;
            shader->set_uniform("view_model_matrix", view_model);
            shader->set_uniform("projection_matrix", projection);
            shader->set_uniform("view_normal_matrix", Matrix3d(view_model.linear().inverse().transpose()));
            if (m_colored_models.empty()) m_model.render();
            else for (const auto& model : m_colored_models) model->render();
            if (m_can_paint && m_selector) {
                if (m_overlay_dirty) {
                    m_overlay.reset();
                    auto selected = m_selector->get_facets_strict(EnforcerBlockerType::ENFORCER);
                    if (!selected.indices.empty()) {
                        m_overlay.init_from(selected);
                        m_overlay.set_color(ColorRGBA(.2f, 1.f, .45f, .75f));
                    }
                    m_overlay_dirty = false;
                }
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-1.f, -1.f);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                m_overlay.render();
                glDisable(GL_BLEND);
                glDisable(GL_POLYGON_OFFSET_FILL);
            }
            rendered = true;
            shader->stop_using();
            if (m_can_paint && m_source && m_mesh.get() == &m_source->its) {
                if (m_seams_dirty) rebuild_seam_models();
                if (auto* flat = wxGetApp().get_shader("flat")) {
                    flat->start_using();
                    flat->set_uniform("view_model_matrix",view_model);
                    flat->set_uniform("projection_matrix",projection);
                    glDisable(GL_DEPTH_TEST);
                    m_seam_model.render();
                    if (m_tool >= 5) { m_hover_model.render(); m_anchor_model.render(); }
                    glEnable(GL_DEPTH_TEST);
                    flat->stop_using();
                }
            }
        }
        m_canvas->SwapBuffers();
        if (plugin_texture_smoke_active())
            record_plugin_texture_preview(m_mesh->indices.size(), rendered, glGetError());
    }

    wxGLCanvas* m_canvas = nullptr;
    wxGLContext* m_context = nullptr; // application-owned; the modal dialog closes before app teardown
    GLModel m_model;
    std::vector<std::unique_ptr<GLModel>> m_colored_models;
    std::vector<uint8_t> m_filaments;
    TexturePalette m_palette;
    int m_default_filament = 1;
    std::shared_ptr<const indexed_triangle_set> m_mesh;
    Vec3d m_center = Vec3d::Zero();
    double m_radius = 1., m_yaw = -.6, m_pitch = -.9, m_zoom = 1.;
    wxPoint m_last;
    bool m_dirty = true;
    std::shared_ptr<const TriangleMesh> m_source;
    std::unique_ptr<AABBMesh> m_raycaster;
    std::unique_ptr<TriangleSelector> m_selector;
    Transform3d m_to_world = Transform3d::Identity(), m_display_frame = Transform3d::Identity();
    std::optional<Vec3f> m_previous_hit;
    EnforcerBlockerType m_previous_state = EnforcerBlockerType::NONE;
    std::function<void(const TriangleSelector::TriangleSplittingData&)> m_changed;
    GLModel m_overlay;
    bool m_overlay_dirty = true, m_can_paint = false;
    std::function<void(bool)> m_stroke;
    bool m_stroking = false;
    std::vector<std::pair<int,int>> m_seams;
    std::function<void(const std::vector<std::pair<int,int>>&)> m_seam_changed;
    int m_seam_slot = -1, m_seam_anchor = -1;
    std::pair<int,int> m_seam_hover{-1,-1};
    bool m_seams_dirty = true;
    GLModel m_seam_model, m_hover_model, m_anchor_model;
    int m_tool = 0;
    float m_brush_radius = 1.f, m_fill_angle = 30.f;
};
