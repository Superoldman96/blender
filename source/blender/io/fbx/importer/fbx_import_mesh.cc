/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#include "BKE_attribute.hh"
#include "BKE_key.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_object_deform.h"
#include "BKE_object_types.hh"

#include "BLI_color.hh"
#include "BLI_listbase.h"
#include "BLI_ordered_edge.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "DNA_key_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "IO_fbx.hh"

#include "fbx_import_mesh.hh"

namespace blender::io::fbx {

static const ufbx_skin_deformer *get_skin_from_mesh(const ufbx_mesh *mesh)
{
  if (mesh->skin_deformers.count > 0) {
    const ufbx_skin_deformer *skin = mesh->skin_deformers[0];
    if (skin != nullptr && mesh->num_vertices > 0 && skin->vertices.count == mesh->num_vertices) {
      return skin;
    }
  }
  return nullptr;
}

static void import_vertex_positions(const ufbx_mesh *fmesh, Mesh *mesh)
{
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
#if 0  // @TODO: "bake" skinned meshes
  if (skin != nullptr) {
    /* For a skinned mesh, transform the vertices into bind pose position, in local space. */
    const ufbx_matrix &geom_to_world = fmesh->instances[0]->geometry_to_world;
    ufbx_matrix world_to_geom = ufbx_matrix_invert(&geom_to_world);
    for (int i = 0; i < fmesh->vertex_position.values.count; i++) {
      ufbx_matrix skin_mat = ufbx_get_skin_vertex_matrix(skin, i, &geom_to_world);
      skin_mat = ufbx_matrix_mul(&world_to_geom, &skin_mat);
      ufbx_vec3 val = ufbx_transform_position(&skin_mat, fmesh->vertex_position.values[i]);
      positions[i] = float3(val.x, val.y, val.z);
      //@TODO: skin normals
    }
    return;
  }
#endif

  BLI_assert(positions.size() == fmesh->vertex_position.values.count);
  for (int i = 0; i < fmesh->vertex_position.values.count; i++) {
    ufbx_vec3 val = fmesh->vertex_position.values[i];
    positions[i] = float3(val.x, val.y, val.z);
  }
}

static void import_faces(const ufbx_mesh *fmesh, Mesh *mesh)
{
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  BLI_assert((face_offsets.size() == fmesh->num_faces + 1) ||
             (face_offsets.is_empty() && fmesh->num_faces == 0));
  for (int face_idx = 0; face_idx < fmesh->num_faces; face_idx++) {
    //@TODO: skip < 3 vertex faces?
    const ufbx_face &fface = fmesh->faces[face_idx];
    face_offsets[face_idx] = fface.index_begin;
    for (int i = 0; i < fface.num_indices; i++) {
      int corner_idx = fface.index_begin + i;
      int vidx = fmesh->vertex_indices[corner_idx];
      corner_verts[corner_idx] = vidx;
    }
  }
}

static void import_face_material_indices(const ufbx_mesh *fmesh,
                                         bke::MutableAttributeAccessor &attributes)
{
  if (fmesh->face_material.count == fmesh->num_faces) {
    bke::SpanAttributeWriter<int> materials = attributes.lookup_or_add_for_write_only_span<int>(
        "material_index", bke::AttrDomain::Face);
    for (int i = 0; i < fmesh->face_material.count; i++) {
      materials.span[i] = fmesh->face_material[i];
    }
    materials.finish();
  }
}

static void import_face_smoothing(const ufbx_mesh *fmesh,
                                  bke::MutableAttributeAccessor &attributes)
{
  if (fmesh->face_smoothing.count > 0 && fmesh->face_smoothing.count == fmesh->num_faces) {
    bke::SpanAttributeWriter<bool> smooth = attributes.lookup_or_add_for_write_only_span<bool>(
        "sharp_face", bke::AttrDomain::Face);
    for (int i = 0; i < fmesh->face_smoothing.count; i++) {
      smooth.span[i] = !fmesh->face_smoothing[i];
    }
    smooth.finish();
  }
}

static void import_edges(const ufbx_mesh *fmesh,
                         Mesh *mesh,
                         bke::MutableAttributeAccessor &attributes)
{
  MutableSpan<int2> edges = mesh->edges_for_write();
  BLI_assert(edges.size() == fmesh->num_edges);
  for (int edge_idx = 0; edge_idx < fmesh->num_edges; edge_idx++) {
    const ufbx_edge &fedge = fmesh->edges[edge_idx];
    int va = fmesh->vertex_indices[fedge.a];
    int vb = fmesh->vertex_indices[fedge.b];
    edges[edge_idx] = int2(va, vb);
  }

  /* Calculate any remaining edges, and add them to explicitly imported ones.
   * Note that this clears any per-edge data, so we have to setup edge creases etc.
   * after that. */
  bke::mesh_calc_edges(*mesh, true, false);

  const bool has_edge_creases = fmesh->edge_crease.count > 0 &&
                                fmesh->edge_crease.count == fmesh->num_edges;
  const bool has_edge_smooth = fmesh->edge_smoothing.count > 0 &&
                               fmesh->edge_smoothing.count == fmesh->num_edges;
  if (has_edge_creases || has_edge_smooth) {
    /* The total number of edges in mesh now might be different from number of explicitly
     * imported ones; we have to build mapping from vertex pairs to edge index. */
    Span<int2> edges = mesh->edges();
    Map<OrderedEdge, int> edge_map;
    edge_map.reserve(edges.size());
    for (const int i : edges.index_range()) {
      edge_map.add(edges[i], i);
    }

    if (has_edge_creases) {
      bke::SpanAttributeWriter<float> creases =
          attributes.lookup_or_add_for_write_only_span<float>("crease_edge",
                                                              bke::AttrDomain::Edge);
      creases.span.fill(0.0f);
      for (int i = 0; i < fmesh->num_edges; i++) {
        const ufbx_edge &fedge = fmesh->edges[i];
        int va = fmesh->vertex_indices[fedge.a];
        int vb = fmesh->vertex_indices[fedge.b];
        int edge_i = edge_map.lookup_default({va, vb}, -1);
        if (edge_i >= 0) {
          /* Python fbx importer was squaring the incoming crease values. */
          creases.span[edge_i] = sqrtf(fmesh->edge_crease[i]);
        }
      }
      creases.finish();
    }

    if (has_edge_smooth) {
      bke::SpanAttributeWriter<bool> sharp = attributes.lookup_or_add_for_write_only_span<bool>(
          "sharp_edge", bke::AttrDomain::Edge);
      sharp.span.fill(false);
      for (int i = 0; i < fmesh->num_edges; i++) {
        const ufbx_edge &fedge = fmesh->edges[i];
        int va = fmesh->vertex_indices[fedge.a];
        int vb = fmesh->vertex_indices[fedge.b];
        int edge_i = edge_map.lookup_default({va, vb}, -1);
        if (edge_i >= 0) {
          sharp.span[edge_i] = !fmesh->edge_smoothing[i];
        }
      }
      sharp.finish();
    }
  }
}

static void import_uvs(const ufbx_mesh *fmesh,
                       bke::MutableAttributeAccessor &attributes,
                       AttributeOwner attr_owner)
{
  for (const ufbx_uv_set &fuv_set : fmesh->uv_sets) {
    std::string attr_name = BKE_attribute_calc_unique_name(attr_owner, fuv_set.name.data);
    bke::SpanAttributeWriter<float2> uvs = attributes.lookup_or_add_for_write_only_span<float2>(
        attr_name, bke::AttrDomain::Corner);
    BLI_assert(fuv_set.vertex_uv.indices.count == uvs.span.size());
    for (int i = 0; i < fuv_set.vertex_uv.indices.count; i++) {
      int val_idx = fuv_set.vertex_uv.indices[i];
      const ufbx_vec2 &uv = fuv_set.vertex_uv.values[val_idx];
      uvs.span[i] = float2(uv.x, uv.y);
    }
    uvs.finish();
  }
}

static void import_colors(const ufbx_mesh *fmesh,
                          Mesh *mesh,
                          bke::MutableAttributeAccessor &attributes,
                          AttributeOwner attr_owner,
                          eFBXVertexColorMode color_mode)
{
  std::string first_color_name;
  for (const ufbx_color_set &fcol_set : fmesh->color_sets) {
    std::string attr_name = BKE_attribute_calc_unique_name(attr_owner, fcol_set.name.data);
    if (first_color_name.empty()) {
      first_color_name = attr_name;
    }
    if (color_mode == eFBXVertexColorMode::sRGB) {
      /* sRGB colors, use 4 bytes per color. */
      bke::SpanAttributeWriter<ColorGeometry4b> cols =
          attributes.lookup_or_add_for_write_only_span<ColorGeometry4b>(attr_name,
                                                                        bke::AttrDomain::Corner);
      BLI_assert(fcol_set.vertex_color.indices.count == cols.span.size());
      for (int i = 0; i < fcol_set.vertex_color.indices.count; i++) {
        int val_idx = fcol_set.vertex_color.indices[i];
        const ufbx_vec4 &col = fcol_set.vertex_color.values[val_idx];
        /* Note: color values are expected to already be in sRGB space. */
        float4 fcol = float4(col.x, col.y, col.z, col.w);
        uchar4 bcol;
        rgba_float_to_uchar(bcol, fcol);
        cols.span[i] = ColorGeometry4b(bcol);
      }
      cols.finish();
    }
    else if (color_mode == eFBXVertexColorMode::Linear) {
      /* Linear colors, use 4 floats per color. */
      bke::SpanAttributeWriter<ColorGeometry4f> cols =
          attributes.lookup_or_add_for_write_only_span<ColorGeometry4f>(attr_name,
                                                                        bke::AttrDomain::Corner);
      BLI_assert(fcol_set.vertex_color.indices.count == cols.span.size());
      for (int i = 0; i < fcol_set.vertex_color.indices.count; i++) {
        int val_idx = fcol_set.vertex_color.indices[i];
        const ufbx_vec4 &col = fcol_set.vertex_color.values[val_idx];
        cols.span[i] = ColorGeometry4f(col.x, col.y, col.z, col.w);
      }
      cols.finish();
    }
    else {
      BLI_assert_unreachable();
    }
  }
  if (!first_color_name.empty()) {
    mesh->active_color_attribute = BLI_strdup(first_color_name.c_str());
    mesh->default_color_attribute = BLI_strdup(first_color_name.c_str());
  }
}

static void import_normals(const ufbx_mesh *fmesh, Mesh *mesh)
{
  if (fmesh->vertex_normal.exists) {
    BLI_assert(fmesh->vertex_normal.indices.count == mesh->corners_num);
    Array<float3> normals(mesh->corners_num);
    for (int i = 0; i < mesh->corners_num; i++) {
      int val_idx = fmesh->vertex_normal.indices[i];
      const ufbx_vec3 &normal = fmesh->vertex_normal.values[val_idx];
      normals[i] = float3(normal.x, normal.y, normal.z);
    }
    bke::mesh_set_custom_normals(*mesh, normals);
  }
}

static void import_skin_vertex_groups(const ufbx_mesh *fmesh,
                                      const ufbx_skin_deformer *skin,
                                      Mesh *mesh)
{
  /* We need to build mapping from cluster indices to non-empty
   * cluster indices. */
  Vector<int> skin_cluster_to_nonempty_cluster_index(skin->clusters.count, -1);
  int cluster_counter = 0;
  for (int i = 0; i < skin->clusters.count; i++) {
    if (skin->clusters[i]->num_weights != 0) {
      skin_cluster_to_nonempty_cluster_index[i] = cluster_counter;
      cluster_counter++;
    }
  }

  MutableSpan<MDeformVert> dverts = mesh->deform_verts_for_write();
  for (int i = 0; i < fmesh->num_vertices; i++) {
    const ufbx_skin_vertex &fvertex = skin->vertices[i];
    int num_weights = fvertex.num_weights;
    if (num_weights > 0) {
      dverts[i].dw = MEM_malloc_arrayN<MDeformWeight>(num_weights, __func__);
      dverts[i].totweight = num_weights;
      for (int j = 0; j < num_weights; j++) {
        const ufbx_skin_weight &fweight = skin->weights[fvertex.weight_begin + j];
        const int bone_index = skin_cluster_to_nonempty_cluster_index[fweight.cluster_index];
        const bool valid = bone_index >= 0;
        dverts[i].dw[j].def_nr = valid ? bone_index : 0;
        dverts[i].dw[j].weight = valid ? fweight.weight : 0.0f;
      }
    }
  }
}

static bool import_blend_shapes(Main &bmain,
                                FbxElementMapping &mapping,
                                const ufbx_mesh *fmesh,
                                Mesh *mesh)
{
  Key *mesh_key = nullptr;
  for (const ufbx_blend_deformer *fdeformer : fmesh->blend_deformers) {
    for (const ufbx_blend_channel *fchan : fdeformer->channels) {
      /* In theory fbx supports multiple keyframes within one blend shape
       * channel; we only take the final target keyframe. */
      if (fchan->target_shape == nullptr) {
        continue;
      }

      if (mesh_key == nullptr) {
        mesh_key = BKE_key_add(&bmain, &mesh->id);
        mesh_key->type = KEY_RELATIVE;
        mesh->key = mesh_key;

        KeyBlock *kb = BKE_keyblock_add(mesh_key, nullptr);
        BKE_keyblock_convert_from_mesh(mesh, mesh_key, kb);
      }

      KeyBlock *kb = BKE_keyblock_add(mesh_key, fchan->target_shape->name.data);
      kb->curval = fchan->weight;
      BKE_keyblock_convert_from_mesh(mesh, mesh_key, kb);
      float3 *kb_data = static_cast<float3 *>(kb->data);
      for (int i = 0; i < fchan->target_shape->num_offsets; i++) {
        int idx = fchan->target_shape->offset_vertices[i];
        const ufbx_vec3 &delta = fchan->target_shape->position_offsets[i];
        kb_data[idx] += float3(delta.x, delta.y, delta.z);
      }

      mapping.el_to_shape_key.add(&fchan->element, mesh_key);
    }
  }
  return mesh_key != nullptr;
}

void import_meshes(Main &bmain,
                   const ufbx_scene &fbx,
                   FbxElementMapping &mapping,
                   const FBXImportParams &params)
{
  for (const ufbx_mesh *fmesh : fbx.meshes) {
    if (fmesh->instances.count == 0) {
      continue; /* Ignore if not used by any objects. */
    }

    const ufbx_skin_deformer *skin = get_skin_from_mesh(fmesh);

    /* Create Mesh outside of main. */
    Mesh *mesh = BKE_mesh_new_nomain(
        fmesh->num_vertices, fmesh->num_edges, fmesh->num_faces, fmesh->num_indices);
    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
    AttributeOwner attr_owner = AttributeOwner::from_id(&mesh->id);

    import_vertex_positions(fmesh, mesh);
    import_faces(fmesh, mesh);
    import_face_material_indices(fmesh, attributes);
    import_face_smoothing(fmesh, attributes);
    import_edges(fmesh, mesh, attributes);
    import_uvs(fmesh, attributes, attr_owner);
    if (params.vertex_colors != eFBXVertexColorMode::None) {
      import_colors(fmesh, mesh, attributes, attr_owner, params.vertex_colors);
    }
    if (params.use_custom_normals) {
      import_normals(fmesh, mesh);
    }
    if (skin != nullptr) {
      import_skin_vertex_groups(fmesh, skin, mesh);
    }

    /* Validate if needed. */
    if (params.validate_meshes) {
      bool verbose_validate = false;
#ifndef NDEBUG
      verbose_validate = true;
#endif
      BKE_mesh_validate(mesh, verbose_validate, false);
    }

    /* Steps below have to be done on the final mesh in Main. */
    Mesh *mesh_main = static_cast<Mesh *>(
        BKE_object_obdata_add_from_type(&bmain, OB_MESH, get_fbx_name(fmesh->name, "Mesh")));
    BKE_mesh_nomain_to_mesh(mesh, mesh_main, nullptr);
    mesh = mesh_main;
    if (params.use_custom_props) {
      read_custom_properties(fmesh->props, mesh->id, params.props_enum_as_string);
    }

    const bool any_shapes = import_blend_shapes(bmain, mapping, fmesh, mesh);

    /* Create objects that use this mesh. */
    for (const ufbx_node *node : fmesh->instances) {
      Object *obj = BKE_object_add_only_object(&bmain, OB_MESH, get_fbx_name(node->name));
      obj->data = mesh_main;
      if (!node->visible) {
        obj->visibility_flag |= OB_HIDE_VIEWPORT;
      }

      if (any_shapes) {
        obj->shapenr = 1;
      }

      bool matrix_already_set = false;

      /* Skinned mesh. */
      if (skin != nullptr && skin->clusters.count > 0) {
        Object *parent_to_arm = nullptr;
        /* Add vertex groups to the object. */
        for (const ufbx_skin_cluster *fcluster : skin->clusters) {
          if (fcluster->num_weights == 0) { /* Do not add groups for empty clusters. */
            continue;
          }
          if (parent_to_arm == nullptr) {
            parent_to_arm = mapping.bone_to_armature.lookup_default(fcluster->bone_node, nullptr);
          }
          std::string bone_name = mapping.node_to_name.lookup_default(fcluster->bone_node, "");
          BKE_object_defgroup_add_name(obj, bone_name.c_str());
        }

        /* Add armature modifier. */
        if (parent_to_arm) {
          ModifierData *md = BKE_modifier_new(eModifierType_Armature);
          STRNCPY(md->name, BKE_id_name(parent_to_arm->id));
          BLI_addtail(&obj->modifiers, md);
          BKE_modifiers_persistent_uid_init(*obj, *md);
          ArmatureModifierData *ad = reinterpret_cast<ArmatureModifierData *>(md);
          ad->object = parent_to_arm;
          obj->parent = parent_to_arm;

          /* We are setting mesh parent to the armature, so set the matrix that is
           * armature-local. */
          ufbx_matrix arm_to_world;
          m44_to_matrix(parent_to_arm->runtime->object_to_world.ptr(), arm_to_world);
          ufbx_matrix world_to_arm = ufbx_matrix_invert(&arm_to_world);
          ufbx_matrix mtx = ufbx_matrix_mul(&node->node_to_world, &node->geometry_to_node);
          mtx = ufbx_matrix_mul(&world_to_arm, &mtx);
          ufbx_matrix_to_obj(mtx, obj);
          matrix_already_set = true;
        }
      }

      /* Assign materials. */
      if (fmesh->materials.count > 0 && node->materials.count == fmesh->materials.count) {
        int mat_index = 0;
        for (int mi = 0; mi < fmesh->materials.count; mi++) {
          const ufbx_material *mesh_fmat = fmesh->materials[mi];
          const ufbx_material *node_fmat = node->materials[mi];
          Material *mesh_mat = mapping.mat_to_material.lookup_default(mesh_fmat, nullptr);
          Material *node_mat = mapping.mat_to_material.lookup_default(node_fmat, nullptr);
          if (mesh_mat != nullptr) {
            mat_index++;
            /* Assign material to the data block. */
            BKE_object_material_assign_single_obdata(&bmain, obj, mesh_mat, mat_index);

            /* If object material is different, assign that to object. */
            if (node_mat != nullptr && node_mat != mesh_mat) {
              BKE_object_material_assign(&bmain, obj, node_mat, mat_index, BKE_MAT_ASSIGN_OBJECT);
            }
          }
        }
        if (mat_index > 0) {
          obj->actcol = 1;
        }
      }

      /* Subdivision. */
      if (params.import_subdivision &&
          fmesh->subdivision_display_mode != UFBX_SUBDIVISION_DISPLAY_DISABLED &&
          (fmesh->subdivision_preview_levels > 0 || fmesh->subdivision_render_levels > 0))
      {
        ModifierData *md = BKE_modifier_new(eModifierType_Subsurf);
        BLI_addtail(&obj->modifiers, md);
        BKE_modifiers_persistent_uid_init(*obj, *md);

        SubsurfModifierData *ssd = reinterpret_cast<SubsurfModifierData *>(md);
        ssd->subdivType = SUBSURF_TYPE_CATMULL_CLARK;
        ssd->levels = fmesh->subdivision_preview_levels;
        ssd->renderLevels = fmesh->subdivision_render_levels;
        ssd->boundary_smooth = fmesh->subdivision_boundary ==
                                       UFBX_SUBDIVISION_BOUNDARY_SHARP_CORNERS ?
                                   SUBSURF_BOUNDARY_SMOOTH_PRESERVE_CORNERS :
                                   SUBSURF_BOUNDARY_SMOOTH_ALL;
      }

      if (params.use_custom_props) {
        read_custom_properties(node->props, obj->id, params.props_enum_as_string);
      }
      if (!matrix_already_set) {
        node_matrix_to_obj(node, obj, mapping);
      }
      mapping.el_to_object.add(&node->element, obj);
    }
  }
}

}  // namespace blender::io::fbx
