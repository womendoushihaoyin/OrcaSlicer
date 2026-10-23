// Migrated from Orca 14a1aa333b6da3ecadf853a4a6285b0f193b7605.
#include "TextureDisplacementParameterize.hpp"
#include <boost/next_prior.hpp>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/measure.h>
#include <CGAL/Polygon_mesh_processing/connected_components.h>
#include <CGAL/Surface_mesh_parameterization/Error_code.h>
#include <CGAL/Surface_mesh_parameterization/LSCM_parameterizer_3.h>
#include <CGAL/Surface_mesh_parameterization/parameterize.h>
#include <CGAL/property_map.h>

namespace Slic3r {
using EpicKernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using _EpicMesh = CGAL::Surface_mesh<EpicKernel::Point_3>;
std::optional<std::vector<Vec2f>> parameterize_texture_lscm(const indexed_triangle_set &mesh)
{
    namespace SMP = CGAL::Surface_mesh_parameterization;

    if (mesh.indices.empty())
        return std::nullopt;

    _EpicMesh cgal_mesh;
    cgal_mesh.reserve(mesh.vertices.size(), mesh.indices.size() * 3 / 2, mesh.indices.size());
    for (const Vec3f &vertex : mesh.vertices) {
        if (!vertex.allFinite())
            return std::nullopt;
        cgal_mesh.add_vertex(EpicKernel::Point_3(vertex.x(), vertex.y(), vertex.z()));
    }
    for (const auto &face : mesh.indices) {
        for (int corner = 0; corner < 3; ++corner)
            if (face[corner] < 0 || size_t(face[corner]) >= mesh.vertices.size())
                return std::nullopt;
        // Refuse invalid/non-manifold input instead of parameterizing a silently dropped face.
        if (cgal_mesh.add_face(_EpicMesh::Vertex_index(face[0]), _EpicMesh::Vertex_index(face[1]),
                               _EpicMesh::Vertex_index(face[2])) == _EpicMesh::null_face())
            return std::nullopt;
    }

    using vertex_descriptor   = boost::graph_traits<_EpicMesh>::vertex_descriptor;
    using halfedge_descriptor = boost::graph_traits<_EpicMesh>::halfedge_descriptor;

    // LSCM assumes a single topological disk: one connected component, one boundary loop. A patch
    // with several disconnected painted islands, or with a hole in it, doesn't qualify -- bail out
    // rather than silently parameterizing just one arbitrary piece of it.
    {
        std::vector<std::size_t> component_id(num_faces(cgal_mesh));
        const std::size_t        num_components = CGAL::Polygon_mesh_processing::connected_components(
            cgal_mesh, CGAL::make_property_map(component_id));
        if (num_components != 1)
            return std::nullopt;
    }

    const halfedge_descriptor border = CGAL::Polygon_mesh_processing::longest_border(cgal_mesh).first;
    if (border == halfedge_descriptor())
        return std::nullopt; // no boundary at all -- a closed patch, which isn't a disk either

    // ...and exactly one boundary loop. One connected component is not enough on its own: a patch with
    // a hole in it (paint a ring, or erase the middle of a stroke) is a single component with two
    // loops, and LSCM will happily "parameterize" it into an overlapping, folded-over chart rather
    // than fail. Walk the border halfedges and check every one of them belongs to the longest loop.
    {
        std::size_t border_halfedges = 0;
        for (halfedge_descriptor h : halfedges(cgal_mesh))
            if (is_border(h, cgal_mesh))
                ++border_halfedges;
        std::size_t loop_length = 0;
        halfedge_descriptor h = border;
        do {
            ++loop_length;
            h = next(h, cgal_mesh);
        } while (h != border && loop_length <= border_halfedges);
        if (loop_length != border_halfedges)
            return std::nullopt; // more than one boundary loop -- not a topological disk
    }

    using Point_2 = EpicKernel::Point_2;
    using UV_pmap = _EpicMesh::Property_map<vertex_descriptor, Point_2>;
    UV_pmap uv_map = cgal_mesh.add_property_map<vertex_descriptor, Point_2>("h:uv", Point_2(0, 0)).first;

    using Parameterizer = SMP::LSCM_parameterizer_3<_EpicMesh>;
    const SMP::Error_code err = SMP::parameterize(cgal_mesh, Parameterizer(), border, uv_map);
    if (err != SMP::OK)
        return std::nullopt;

    // Vertices were added in the exact same order as mesh.vertices above,
    // and Surface_mesh assigns indices sequentially on insertion into a fresh mesh, so a
    // vertex_descriptor's index here is guaranteed to match the original input vertex index --
    // the same assumption cgal_to_indexed_triangle_set() above already relies on.
    std::vector<Vec2f> result(mesh.vertices.size(), Vec2f::Zero());
    for (vertex_descriptor vd : vertices(cgal_mesh)) {
        const std::size_t idx = std::size_t(vd);
        if (idx < result.size()) {
            const Point_2 &uv = uv_map[vd];
            result[idx]        = Vec2f(float(uv.x()), float(uv.y()));
        }
    }
    return result;
}

} // namespace Slic3r
