/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/brushes/brushes.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_brush.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh_brush_common.hh"
#include "editors/sculpt_paint/sculpt_automask.hh"
#include "editors/sculpt_paint/sculpt_intern.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint {

inline namespace clay_strips_cc {

struct LocalData {
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
};

/**
 * Applies a linear falloff based on the z distance in brush local space to the factor.
 *
 * Note: We may want to provide users the ability to change this falloff in the future, the
 * important detail is that we reduce the influence of the brush on vertices that are potentially
 * "deep" inside the cube test area (i.e. on a nearby plane).
 *
 * TODO: Depending on if other brushes begin to use the calc_brush_cube_distances, we may want
 * to consider either inlining this falloff in that method, or making this a commonly accessible
 * function.
 */
BLI_NOINLINE static void apply_z_axis_falloff(const Span<float3> vert_positions,
                                              const Span<int> verts,
                                              const float4x4 &mat,
                                              const MutableSpan<float> factors)
{
  BLI_assert(factors.size() == verts.size());
  for (const int i : factors.index_range()) {
    const float local_z_distance = math::abs(
        math::transform_point(mat, vert_positions[verts[i]]).z);
    factors[i] *= 1 - local_z_distance;
  }
}

BLI_NOINLINE static void apply_z_axis_falloff(const Span<float3> positions,
                                              const float4x4 &mat,
                                              const MutableSpan<float> factors)
{
  BLI_assert(factors.size() == positions.size());
  for (const int i : factors.index_range()) {
    const float local_z_distance = math::abs(math::transform_point(mat, positions[i]).z);
    factors[i] *= 1 - local_z_distance;
  }
}

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const float4x4 &mat,
                       const float4 &plane,
                       const float strength,
                       const bool flip,
                       const Span<float3> vert_normals,
                       const MeshAttributeData &attribute_data,
                       const bke::pbvh::MeshNode &node,
                       Object &object,
                       LocalData &tls,
                       const PositionDeformData &position_data)
{
  SculptSession &ss = *object.sculpt;
  const StrokeCache &cache = *ss.cache;

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, position_data.eval, verts, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_cube_distances(brush, mat, position_data.eval, verts, distances, factors);
  apply_z_axis_falloff(position_data.eval, verts, mat, factors);
  filter_distances_with_radius(1.0f, distances, factors);
  apply_hardness_to_distances(1.0f, cache.hardness, distances);
  BKE_brush_calc_curve_factors(
      eBrushCurvePreset(brush.curve_preset), brush.curve, distances, 1.0f, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, position_data.eval, verts, factors);

  scale_factors(factors, strength);

  if (flip) {
    filter_below_plane_factors(position_data.eval, verts, plane, factors);
  }
  else {
    filter_above_plane_factors(position_data.eval, verts, plane, factors);
  }

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  calc_translations_to_plane(position_data.eval, verts, plane, translations);
  filter_plane_trim_limit_factors(brush, cache, translations, factors);
  scale_translations(translations, factors);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float4x4 &mat,
                       const float4 &plane,
                       const float strength,
                       const bool flip,
                       const bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.sculpt;
  const StrokeCache &cache = *ss.cache;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  tls.factors.resize(positions.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, subdiv_ccg, grids, factors);
  }

  tls.distances.resize(positions.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_cube_distances(brush, mat, positions, distances, factors);
  apply_z_axis_falloff(positions, mat, factors);
  filter_distances_with_radius(1.0f, distances, factors);
  apply_hardness_to_distances(1.0f, cache.hardness, distances);
  BKE_brush_calc_curve_factors(
      eBrushCurvePreset(brush.curve_preset), brush.curve, distances, 1.0f, factors);

  auto_mask::calc_grids_factors(depsgraph, object, cache.automasking.get(), node, grids, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);

  scale_factors(factors, strength);

  if (flip) {
    filter_below_plane_factors(positions, plane, factors);
  }
  else {
    filter_above_plane_factors(positions, plane, factors);
  }

  tls.translations.resize(positions.size());
  const MutableSpan<float3> translations = tls.translations;
  calc_translations_to_plane(positions, plane, translations);
  filter_plane_trim_limit_factors(brush, cache, translations, factors);
  scale_translations(translations, factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float4x4 &mat,
                       const float4 &plane,
                       const float strength,
                       const bool flip,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.sculpt;
  const StrokeCache &cache = *ss.cache;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(*ss.bm, verts, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, verts, factors);
  }

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_cube_distances(brush, mat, positions, distances, factors);
  apply_z_axis_falloff(positions, mat, factors);
  filter_distances_with_radius(1.0f, distances, factors);
  apply_hardness_to_distances(1.0f, cache.hardness, distances);
  BKE_brush_calc_curve_factors(
      eBrushCurvePreset(brush.curve_preset), brush.curve, distances, 1.0f, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);

  scale_factors(factors, strength);

  if (flip) {
    filter_below_plane_factors(positions, plane, factors);
  }
  else {
    filter_above_plane_factors(positions, plane, factors);
  }

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  calc_translations_to_plane(positions, plane, translations);
  filter_plane_trim_limit_factors(brush, cache, translations, factors);
  scale_translations(translations, factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, verts);
}

}  // namespace clay_strips_cc

/**
 * Basic principles of the clay strips brush:
 * * Calculate a brush plane from an initial node mask
 * * Use this center position and normal to create a brush-local matrix
 * * Use this matrix and the plane to calculate and use cube distances for
 * * the affected area
 */
void do_clay_strips_brush(const Depsgraph &depsgraph,
                          const Sculpt &sd,
                          Object &object,
                          const IndexMask &node_mask,
                          const float3 &plane_normal,
                          const float3 &plane_center)
{
  SculptSession &ss = *object.sculpt;
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const bool flip = (ss.cache->bstrength < 0.0f);

  /* Note: This return has to happen *after* the call to calc_brush_plane for now, as
   * the method is not idempotent and sets variables inside the stroke cache. */
  if (math::is_zero(ss.cache->grab_delta_symm)) {
    return;
  }

  float4x4 mat = float4x4::identity();
  mat.x_axis() = math::cross(plane_normal, ss.cache->grab_delta_symm);
  mat.y_axis() = math::cross(plane_normal, float3(mat[0]));
  mat.z_axis() = plane_normal;
  mat.location() = plane_center;
  mat = math::normalize(mat);

  /* Scale brush local space matrix. */
  const float4x4 scale = math::from_scale<float4x4>(float3(ss.cache->radius));
  float4x4 tmat = mat * scale;
  tmat.y_axis() *= brush.tip_scale_x;
  mat = math::invert(tmat);

  float4 plane;
  plane_from_point_normal_v3(plane, plane_center, plane_normal);

  const float strength = std::abs(ss.cache->bstrength);

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *static_cast<Mesh *>(object.data);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      const PositionDeformData position_data(depsgraph, object);
      const MeshAttributeData attribute_data(mesh);
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);
      node_mask.foreach_index(GrainSize(1), [&](const int i) {
        LocalData &tls = all_tls.local();
        calc_faces(depsgraph,
                   sd,
                   brush,
                   mat,
                   plane,
                   strength,
                   flip,
                   vert_normals,
                   attribute_data,
                   nodes[i],
                   object,
                   tls,
                   position_data);
        bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;
      MutableSpan<float3> positions = subdiv_ccg.positions;
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      node_mask.foreach_index(GrainSize(1), [&](const int i) {
        LocalData &tls = all_tls.local();
        calc_grids(depsgraph, sd, object, brush, mat, plane, strength, flip, nodes[i], tls);
        bke::pbvh::update_node_bounds_grids(subdiv_ccg.grid_area, positions, nodes[i]);
      });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      node_mask.foreach_index(GrainSize(1), [&](const int i) {
        LocalData &tls = all_tls.local();
        calc_bmesh(depsgraph, sd, object, brush, mat, plane, strength, flip, nodes[i], tls);
        bke::pbvh::update_node_bounds_bmesh(nodes[i]);
      });
      break;
    }
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
}

namespace brushes::clay_strips {
NodeMaskResult calc_node_mask(const Depsgraph &depsgraph,
                              Object &object,
                              const Brush &brush,
                              IndexMaskMemory &memory)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const SculptSession &ss = *object.sculpt;

  const bool flip = (ss.cache->bstrength < 0.0f);
  const float offset = brush_plane_offset_get(brush, ss);
  const float displace = ss.cache->radius * (0.18f + offset) * (flip ? -1.0f : 1.0f);

  /* TODO: Test to see if the sqrt2 extra factor can be removed */
  const float initial_radius_squared = math::square(ss.cache->radius * math::numbers::sqrt2);

  const bool use_original = !ss.cache->accum;
  const IndexMask initial_node_mask = gather_nodes(pbvh,
                                                   eBrushFalloffShape(brush.falloff_shape),
                                                   use_original,
                                                   ss.cache->location_symm,
                                                   initial_radius_squared,
                                                   ss.cache->view_normal_symm,
                                                   memory);

  float3 plane_center;
  float3 sculpt_plane_normal;
  calc_brush_plane(depsgraph, brush, object, initial_node_mask, sculpt_plane_normal, plane_center);

  float3 plane_normal = sculpt_plane_normal;
  /* Ignore brush settings and recalculate the area normal. */
  if (brush.sculpt_plane != SCULPT_DISP_DIR_AREA || (brush.flag & BRUSH_ORIGINAL_NORMAL)) {
    plane_normal =
        calc_area_normal(depsgraph, brush, object, initial_node_mask).value_or(float3(0));
  }

  plane_normal = tilt_apply_to_normal(plane_normal, *ss.cache, brush.tilt_strength_factor);
  plane_center += plane_normal * ss.cache->scale * displace;

  /* With a cube influence area, this brush needs slightly more than the radius.
   *
   * SQRT3 because the cube circumscribes the spherical brush area, so the current radius is equal
   * to half of the length of a side of the cube. */
  const float radius_squared = math::square(ss.cache->radius * math::numbers::sqrt3);
  const IndexMask plane_mask = bke::pbvh::search_nodes(
      pbvh, memory, [&](const bke::pbvh::Node &node) {
        if (node_fully_masked_or_hidden(node)) {
          return false;
        }
        return node_in_sphere(node, plane_center, radius_squared, use_original);
      });

  return {plane_mask, plane_center, plane_normal};
}
}  // namespace brushes::clay_strips

}  // namespace blender::ed::sculpt_paint
