/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 */

#include <cmath>

#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_node_legacy_types.hh"

#include "ED_gizmo_library.hh"
#include "ED_screen.hh"

#include "IMB_imbuf_types.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_types.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

/* -------------------------------------------------------------------- */
/** \name Local Utilities
 * \{ */

static void node_gizmo_calc_matrix_space(const SpaceNode *snode,
                                         const ARegion *region,
                                         float matrix_space[4][4])
{
  unit_m4(matrix_space);
  mul_v3_fl(matrix_space[0], snode->zoom);
  mul_v3_fl(matrix_space[1], snode->zoom);
  matrix_space[3][0] = (region->winx / 2) + snode->xof;
  matrix_space[3][1] = (region->winy / 2) + snode->yof;
}

static void node_gizmo_calc_matrix_space_with_image_dims(const SpaceNode *snode,
                                                         const ARegion *region,
                                                         const float2 &image_dims,
                                                         const float2 &image_offset,
                                                         float matrix_space[4][4])
{
  unit_m4(matrix_space);
  mul_v3_fl(matrix_space[0], snode->zoom * image_dims.x);
  mul_v3_fl(matrix_space[1], snode->zoom * image_dims.y);
  matrix_space[3][0] = ((region->winx / 2) + snode->xof) -
                       ((image_dims.x / 2.0f - image_offset.x) * snode->zoom);
  matrix_space[3][1] = ((region->winy / 2) + snode->yof) -
                       ((image_dims.y / 2.0f - image_offset.y) * snode->zoom);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Backdrop Gizmo
 * \{ */

static void gizmo_node_backdrop_prop_matrix_get(const wmGizmo * /*gz*/,
                                                wmGizmoProperty *gz_prop,
                                                void *value_p)
{
  float(*matrix)[4] = (float(*)[4])value_p;
  BLI_assert(gz_prop->type->array_length == 16);
  const SpaceNode *snode = (const SpaceNode *)gz_prop->custom_func.user_data;
  matrix[0][0] = snode->zoom;
  matrix[1][1] = snode->zoom;
  matrix[3][0] = snode->xof;
  matrix[3][1] = snode->yof;
}

static void gizmo_node_backdrop_prop_matrix_set(const wmGizmo * /*gz*/,
                                                wmGizmoProperty *gz_prop,
                                                const void *value_p)
{
  const float(*matrix)[4] = (const float(*)[4])value_p;
  BLI_assert(gz_prop->type->array_length == 16);
  SpaceNode *snode = (SpaceNode *)gz_prop->custom_func.user_data;
  snode->zoom = matrix[0][0];
  snode->xof = matrix[3][0];
  snode->yof = matrix[3][1];
}

static bool WIDGETGROUP_node_transform_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);

  if ((snode->flag & SNODE_BACKDRAW) == 0) {
    return false;
  }

  if (snode && snode->edittree && snode->edittree->type == NTREE_COMPOSIT) {
    bNode *node = bke::node_get_active(*snode->edittree);

    if (node && ELEM(node->type_legacy, CMP_NODE_VIEWER)) {
      return true;
    }
  }

  return false;
}

static void WIDGETGROUP_node_transform_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  wmGizmoWrapper *wwrapper = (wmGizmoWrapper *)MEM_mallocN(sizeof(wmGizmoWrapper), __func__);

  wwrapper->gizmo = WM_gizmo_new("GIZMO_GT_cage_2d", gzgroup, nullptr);

  RNA_enum_set(wwrapper->gizmo->ptr,
               "transform",
               ED_GIZMO_CAGE_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE_XFORM_FLAG_SCALE_UNIFORM);

  gzgroup->customdata = wwrapper;
}

static void WIDGETGROUP_node_transform_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  Main *bmain = CTX_data_main(C);
  wmGizmo *cage = ((wmGizmoWrapper *)gzgroup->customdata)->gizmo;
  const ARegion *region = CTX_wm_region(C);
  /* center is always at the origin */
  const float origin[3] = {float(region->winx / 2), float(region->winy / 2), 0.0f};

  void *lock;
  Image *ima = BKE_image_ensure_viewer(bmain, IMA_TYPE_COMPOSITE, "Viewer Node");
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, nullptr, &lock);

  if (ibuf) {
    const float2 dims = {
        (ibuf->x > 0) ? ibuf->x : 64.0f,
        (ibuf->y > 0) ? ibuf->y : 64.0f,
    };

    RNA_float_set_array(cage->ptr, "dimensions", dims);
    WM_gizmo_set_matrix_location(cage, origin);
    WM_gizmo_set_flag(cage, WM_GIZMO_HIDDEN, false);

    /* Need to set property here for undo. TODO: would prefer to do this in _init. */
    SpaceNode *snode = CTX_wm_space_node(C);
#if 0
    PointerRNA nodeptr = RNA_pointer_create_discrete(snode->id, &RNA_SpaceNodeEditor, snode);
    WM_gizmo_target_property_def_rna(cage, "offset", &nodeptr, "backdrop_offset", -1);
    WM_gizmo_target_property_def_rna(cage, "scale", &nodeptr, "backdrop_zoom", -1);
#endif

    wmGizmoPropertyFnParams params{};
    params.value_get_fn = gizmo_node_backdrop_prop_matrix_get;
    params.value_set_fn = gizmo_node_backdrop_prop_matrix_set;
    params.range_get_fn = nullptr;
    params.user_data = snode;
    WM_gizmo_target_property_def_func(cage, "matrix", &params);
  }
  else {
    WM_gizmo_set_flag(cage, WM_GIZMO_HIDDEN, true);
  }

  BKE_image_release_ibuf(ima, ibuf, lock);
}

void NODE_GGT_backdrop_transform(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Backdrop Transform Widget";
  gzgt->idname = "NODE_GGT_backdrop_transform";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = WIDGETGROUP_node_transform_poll;
  gzgt->setup = WIDGETGROUP_node_transform_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = WIDGETGROUP_node_transform_refresh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Crop Gizmo
 * \{ */

struct NodeBBoxWidgetGroup {
  wmGizmo *border;

  struct {
    float2 dims;
    float2 offset;
  } state;

  struct {
    PointerRNA ptr;
    PropertyRNA *prop;
    bContext *context;
  } update_data;
};

static void gizmo_node_bbox_update(NodeBBoxWidgetGroup *bbox_group)
{
  RNA_property_update(
      bbox_group->update_data.context, &bbox_group->update_data.ptr, bbox_group->update_data.prop);
}

static void two_xy_to_rect(
    const NodeTwoXYs *nxy, const float2 &dims, const float2 offset, bool is_relative, rctf *r_rect)
{
  if (is_relative) {
    r_rect->xmin = nxy->fac_x1 + (offset.x / dims.x);
    r_rect->xmax = nxy->fac_x2 + (offset.x / dims.x);
    r_rect->ymin = nxy->fac_y2 + (offset.y / dims.y);
    r_rect->ymax = nxy->fac_y1 + (offset.y / dims.y);
  }
  else {
    r_rect->xmin = (nxy->x1 + offset.x) / dims.x;
    r_rect->xmax = (nxy->x2 + offset.x) / dims.x;
    r_rect->ymin = (nxy->y2 + offset.y) / dims.y;
    r_rect->ymax = (nxy->y1 + offset.y) / dims.y;
  }
}

static void two_xy_from_rect(
    NodeTwoXYs *nxy, const rctf *rect, const float2 &dims, const float2 &offset, bool is_relative)
{
  if (is_relative) {
    nxy->fac_x1 = rect->xmin - (offset.x / dims.x);
    nxy->fac_x2 = rect->xmax - (offset.x / dims.x);
    nxy->fac_y2 = rect->ymin - (offset.y / dims.y);
    nxy->fac_y1 = rect->ymax - (offset.y / dims.y);
  }
  else {
    nxy->x1 = rect->xmin * dims.x - offset.x;
    nxy->x2 = rect->xmax * dims.x - offset.x;
    nxy->y2 = rect->ymin * dims.y - offset.y;
    nxy->y1 = rect->ymax * dims.y - offset.y;
  }
}

/* scale callbacks */
static void gizmo_node_crop_prop_matrix_get(const wmGizmo *gz,
                                            wmGizmoProperty *gz_prop,
                                            void *value_p)
{
  float(*matrix)[4] = (float(*)[4])value_p;
  BLI_assert(gz_prop->type->array_length == 16);
  NodeBBoxWidgetGroup *crop_group = (NodeBBoxWidgetGroup *)gz->parent_gzgroup->customdata;
  const float2 dims = crop_group->state.dims;
  const float2 offset = crop_group->state.offset;
  const bNode *node = (const bNode *)gz_prop->custom_func.user_data;
  const NodeTwoXYs *nxy = (const NodeTwoXYs *)node->storage;
  bool is_relative = bool(node->custom2);
  rctf rct;
  two_xy_to_rect(nxy, dims, offset, is_relative, &rct);
  matrix[0][0] = fabsf(BLI_rctf_size_x(&rct));
  matrix[1][1] = fabsf(BLI_rctf_size_y(&rct));
  matrix[3][0] = (BLI_rctf_cent_x(&rct) - 0.5f) * dims[0];
  matrix[3][1] = (BLI_rctf_cent_y(&rct) - 0.5f) * dims[1];
}

static void gizmo_node_crop_prop_matrix_set(const wmGizmo *gz,
                                            wmGizmoProperty *gz_prop,
                                            const void *value_p)
{
  const float(*matrix)[4] = (const float(*)[4])value_p;
  BLI_assert(gz_prop->type->array_length == 16);
  NodeBBoxWidgetGroup *crop_group = (NodeBBoxWidgetGroup *)gz->parent_gzgroup->customdata;
  const float2 dims = crop_group->state.dims;
  const float2 offset = crop_group->state.offset;
  bNode *node = (bNode *)gz_prop->custom_func.user_data;
  NodeTwoXYs *nxy = (NodeTwoXYs *)node->storage;
  bool is_relative = bool(node->custom2);
  rctf rct;
  two_xy_to_rect(nxy, dims, offset, is_relative, &rct);
  BLI_rctf_resize(&rct, fabsf(matrix[0][0]), fabsf(matrix[1][1]));
  BLI_rctf_recenter(&rct, ((matrix[3][0]) / dims[0]) + 0.5f, ((matrix[3][1]) / dims[1]) + 0.5f);
  rctf rct_isect{};
  rct_isect.xmin = offset.x / dims.x;
  rct_isect.xmax = offset.x / dims.x + 1;
  rct_isect.ymin = offset.y;
  rct_isect.ymax = offset.y / dims.y + 1;
  BLI_rctf_isect(&rct_isect, &rct, &rct);
  two_xy_from_rect(nxy, &rct, dims, offset, is_relative);
  gizmo_node_bbox_update(crop_group);
}

static bool WIDGETGROUP_node_crop_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);

  if ((snode->flag & SNODE_BACKDRAW) == 0) {
    return false;
  }

  if (snode && snode->edittree && snode->edittree->type == NTREE_COMPOSIT) {
    bNode *node = bke::node_get_active(*snode->edittree);

    if (node && ELEM(node->type_legacy, CMP_NODE_CROP)) {
      /* ignore 'use_crop_size', we can't usefully edit the crop in this case. */
      if ((node->custom1 & (1 << 0)) == 0) {
        return true;
      }
    }
  }

  return false;
}

static void WIDGETGROUP_node_crop_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  NodeBBoxWidgetGroup *crop_group = MEM_new<NodeBBoxWidgetGroup>(__func__);
  crop_group->border = WM_gizmo_new("GIZMO_GT_cage_2d", gzgroup, nullptr);

  RNA_enum_set(crop_group->border->ptr,
               "transform",
               ED_GIZMO_CAGE_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE_XFORM_FLAG_SCALE);

  gzgroup->customdata = crop_group;
  gzgroup->customdata_free = [](void *customdata) {
    MEM_delete(static_cast<NodeBBoxWidgetGroup *>(customdata));
  };
}

static void WIDGETGROUP_node_crop_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  ARegion *region = CTX_wm_region(C);
  wmGizmo *gz = (wmGizmo *)gzgroup->gizmos.first;

  SpaceNode *snode = CTX_wm_space_node(C);

  node_gizmo_calc_matrix_space(snode, region, gz->matrix_space);
}

static void WIDGETGROUP_node_crop_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  Main *bmain = CTX_data_main(C);
  SpaceNode *snode = CTX_wm_space_node(C);

  NodeBBoxWidgetGroup *crop_group = (NodeBBoxWidgetGroup *)gzgroup->customdata;
  wmGizmo *gz = crop_group->border;

  void *lock;
  Image *ima = BKE_image_ensure_viewer(bmain, IMA_TYPE_COMPOSITE, "Viewer Node");
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, nullptr, &lock);

  if (ibuf) {
    crop_group->state.dims[0] = (ibuf->x > 0) ? ibuf->x : 64.0f;
    crop_group->state.dims[1] = (ibuf->y > 0) ? ibuf->y : 64.0f;
    copy_v2_v2(crop_group->state.offset, ima->runtime.backdrop_offset);

    RNA_float_set_array(gz->ptr, "dimensions", crop_group->state.dims);
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);

    bNode *node = bke::node_get_active(*snode->edittree);

    crop_group->update_data.context = (bContext *)C;
    crop_group->update_data.ptr = RNA_pointer_create_discrete(
        (ID *)snode->edittree, &RNA_CompositorNodeCrop, node);
    crop_group->update_data.prop = RNA_struct_find_property(&crop_group->update_data.ptr,
                                                            "relative");

    wmGizmoPropertyFnParams params{};
    params.value_get_fn = gizmo_node_crop_prop_matrix_get;
    params.value_set_fn = gizmo_node_crop_prop_matrix_set;
    params.range_get_fn = nullptr;
    params.user_data = node;
    WM_gizmo_target_property_def_func(gz, "matrix", &params);
  }
  else {
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  }

  BKE_image_release_ibuf(ima, ibuf, lock);
}

void NODE_GGT_backdrop_crop(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Backdrop Crop Widget";
  gzgt->idname = "NODE_GGT_backdrop_crop";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = WIDGETGROUP_node_crop_poll;
  gzgt->setup = WIDGETGROUP_node_crop_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->draw_prepare = WIDGETGROUP_node_crop_draw_prepare;
  gzgt->refresh = WIDGETGROUP_node_crop_refresh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Box Mask
 * \{ */

static void gizmo_node_box_mask_prop_matrix_get(const wmGizmo *gz,
                                                wmGizmoProperty *gz_prop,
                                                void *value_p)
{
  float(*matrix)[4] = (float(*)[4])value_p;
  BLI_assert(gz_prop->type->array_length == 16);
  NodeBBoxWidgetGroup *mask_group = (NodeBBoxWidgetGroup *)gz->parent_gzgroup->customdata;
  const float2 dims = mask_group->state.dims;
  const float2 offset = mask_group->state.offset;
  const bNode *node = (const bNode *)gz_prop->custom_func.user_data;
  const NodeBoxMask *mask_node = (const NodeBoxMask *)node->storage;
  const float aspect = dims.x / dims.y;

  float loc[3], rot[3][3], size[3];
  mat4_to_loc_rot_size(loc, rot, size, matrix);

  axis_angle_to_mat3_single(rot, 'Z', mask_node->rotation);

  loc[0] = (mask_node->x - 0.5) * dims.x + offset.x;
  loc[1] = (mask_node->y - 0.5) * dims.y + offset.y;
  loc[2] = 0;

  size[0] = mask_node->width;
  size[1] = mask_node->height * aspect;
  size[2] = 1;

  loc_rot_size_to_mat4(matrix, loc, rot, size);
}

static void gizmo_node_box_mask_prop_matrix_set(const wmGizmo *gz,
                                                wmGizmoProperty *gz_prop,
                                                const void *value_p)
{
  const float(*matrix)[4] = (const float(*)[4])value_p;
  BLI_assert(gz_prop->type->array_length == 16);
  NodeBBoxWidgetGroup *mask_group = (NodeBBoxWidgetGroup *)gz->parent_gzgroup->customdata;
  const float2 dims = mask_group->state.dims;
  const float2 offset = mask_group->state.offset;
  bNode *node = (bNode *)gz_prop->custom_func.user_data;
  NodeBoxMask *mask_node = (NodeBoxMask *)node->storage;

  const float aspect = dims.x / dims.y;
  rctf rct;
  rct.xmin = mask_node->x - mask_node->width / 2;
  rct.xmax = mask_node->x + mask_node->width / 2;
  rct.ymin = mask_node->y - mask_node->height / 2;
  rct.ymax = mask_node->y + mask_node->height / 2;

  float loc[3];
  float rot[3][3];
  float size[3];
  mat4_to_loc_rot_size(loc, rot, size, matrix);

  float eul[3];

  /* Rotation can't be extracted from matrix when the gizmo width or height is zero. */
  if (size[0] != 0 and size[1] != 0) {
    mat4_to_eul(eul, matrix);
    mask_node->rotation = eul[2];
  }

  BLI_rctf_resize(&rct, fabsf(size[0]), fabsf(size[1]) / aspect);
  BLI_rctf_recenter(
      &rct, ((loc[0] - offset.x) / dims.x) + 0.5, ((loc[1] - offset.y) / dims.y) + 0.5);

  mask_node->width = size[0];
  mask_node->height = size[1] / aspect;
  mask_node->x = rct.xmin + mask_node->width / 2;
  mask_node->y = rct.ymin + mask_node->height / 2;

  gizmo_node_bbox_update(mask_group);
}

static bool WIDGETGROUP_node_box_mask_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);

  if ((snode->flag & SNODE_BACKDRAW) == 0) {
    return false;
  }

  if (snode && snode->edittree && snode->edittree->type == NTREE_COMPOSIT) {
    bNode *node = bke::node_get_active(*snode->edittree);

    if (node && ELEM(node->type_legacy, CMP_NODE_MASK_BOX)) {
      return true;
    }
  }

  return false;
}

static void WIDGETGROUP_node_box_mask_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  NodeBBoxWidgetGroup *mask_group = MEM_new<NodeBBoxWidgetGroup>(__func__);
  mask_group->border = WM_gizmo_new("GIZMO_GT_cage_2d", gzgroup, nullptr);

  RNA_enum_set(mask_group->border->ptr,
               "transform",
               ED_GIZMO_CAGE_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE_XFORM_FLAG_ROTATE |
                   ED_GIZMO_CAGE_XFORM_FLAG_SCALE);

  gzgroup->customdata = mask_group;
  gzgroup->customdata_free = [](void *customdata) {
    MEM_delete(static_cast<NodeBBoxWidgetGroup *>(customdata));
  };
}

static void WIDGETGROUP_node_box_mask_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  ARegion *region = CTX_wm_region(C);
  wmGizmo *gz = (wmGizmo *)gzgroup->gizmos.first;

  SpaceNode *snode = CTX_wm_space_node(C);

  node_gizmo_calc_matrix_space(snode, region, gz->matrix_space);
}

static void WIDGETGROUP_node_box_mask_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  Main *bmain = CTX_data_main(C);
  NodeBBoxWidgetGroup *mask_group = (NodeBBoxWidgetGroup *)gzgroup->customdata;
  wmGizmo *gz = mask_group->border;

  void *lock;
  Image *ima = BKE_image_ensure_viewer(bmain, IMA_TYPE_COMPOSITE, "Render Result");
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, nullptr, &lock);

  if (ibuf) {
    mask_group->state.dims[0] = (ibuf->x > 0) ? ibuf->x : 64.0f;
    mask_group->state.dims[1] = (ibuf->y > 0) ? ibuf->y : 64.0f;
    copy_v2_v2(mask_group->state.offset, ima->runtime.backdrop_offset);

    RNA_float_set_array(gz->ptr, "dimensions", mask_group->state.dims);
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);

    SpaceNode *snode = CTX_wm_space_node(C);
    bNode *node = bke::node_get_active(*snode->edittree);

    mask_group->update_data.context = (bContext *)C;
    mask_group->update_data.ptr = RNA_pointer_create_discrete(
        (ID *)snode->edittree, &RNA_CompositorNodeCrop, node);
    mask_group->update_data.prop = RNA_struct_find_property(&mask_group->update_data.ptr, "x");

    wmGizmoPropertyFnParams params{};
    params.value_get_fn = gizmo_node_box_mask_prop_matrix_get;
    params.value_set_fn = gizmo_node_box_mask_prop_matrix_set;
    params.range_get_fn = nullptr;
    params.user_data = node;
    WM_gizmo_target_property_def_func(gz, "matrix", &params);
  }
  else {
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  }

  BKE_image_release_ibuf(ima, ibuf, lock);
}

void NODE_GGT_backdrop_box_mask(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Backdrop Box Mask Widget";
  gzgt->idname = "NODE_GGT_backdrop_box_mask";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = WIDGETGROUP_node_box_mask_poll;
  gzgt->setup = WIDGETGROUP_node_box_mask_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->draw_prepare = WIDGETGROUP_node_box_mask_draw_prepare;
  gzgt->refresh = WIDGETGROUP_node_box_mask_refresh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sun Beams
 * \{ */

struct NodeSunBeamsWidgetGroup {
  wmGizmo *gizmo;

  struct {
    float2 dims;
    float2 offset;
  } state;
};

static bool WIDGETGROUP_node_sbeam_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);

  if ((snode->flag & SNODE_BACKDRAW) == 0) {
    return false;
  }

  if (snode && snode->edittree && snode->edittree->type == NTREE_COMPOSIT) {
    bNode *node = bke::node_get_active(*snode->edittree);

    if (node && ELEM(node->type_legacy, CMP_NODE_SUNBEAMS)) {
      return true;
    }
  }

  return false;
}

static void WIDGETGROUP_node_sbeam_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  NodeSunBeamsWidgetGroup *sbeam_group = (NodeSunBeamsWidgetGroup *)MEM_mallocN(
      sizeof(NodeSunBeamsWidgetGroup), __func__);

  sbeam_group->gizmo = WM_gizmo_new("GIZMO_GT_move_3d", gzgroup, nullptr);
  wmGizmo *gz = sbeam_group->gizmo;

  RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_MOVE_STYLE_CROSS_2D);

  gz->scale_basis = 0.05f / 75.0f;

  gzgroup->customdata = sbeam_group;
}

static void WIDGETGROUP_node_sbeam_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  NodeSunBeamsWidgetGroup *sbeam_group = (NodeSunBeamsWidgetGroup *)gzgroup->customdata;
  ARegion *region = CTX_wm_region(C);
  wmGizmo *gz = (wmGizmo *)gzgroup->gizmos.first;

  SpaceNode *snode = CTX_wm_space_node(C);

  node_gizmo_calc_matrix_space_with_image_dims(
      snode, region, sbeam_group->state.dims, sbeam_group->state.offset, gz->matrix_space);
}

static void WIDGETGROUP_node_sbeam_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  Main *bmain = CTX_data_main(C);
  NodeSunBeamsWidgetGroup *sbeam_group = (NodeSunBeamsWidgetGroup *)gzgroup->customdata;
  wmGizmo *gz = sbeam_group->gizmo;

  void *lock;
  Image *ima = BKE_image_ensure_viewer(bmain, IMA_TYPE_COMPOSITE, "Viewer Node");
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, nullptr, &lock);

  if (ibuf) {
    sbeam_group->state.dims[0] = (ibuf->x > 0) ? ibuf->x : 64.0f;
    sbeam_group->state.dims[1] = (ibuf->y > 0) ? ibuf->y : 64.0f;
    copy_v2_v2(sbeam_group->state.offset, ima->runtime.backdrop_offset);

    SpaceNode *snode = CTX_wm_space_node(C);
    bNode *node = bke::node_get_active(*snode->edittree);

    /* Need to set property here for undo. TODO: would prefer to do this in _init. */
    PointerRNA nodeptr = RNA_pointer_create_discrete(
        (ID *)snode->edittree, &RNA_CompositorNodeSunBeams, node);
    WM_gizmo_target_property_def_rna(gz, "offset", &nodeptr, "source", -1);

    WM_gizmo_set_flag(gz, WM_GIZMO_DRAW_MODAL, true);
  }
  else {
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  }

  BKE_image_release_ibuf(ima, ibuf, lock);
}

void NODE_GGT_backdrop_sun_beams(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Sun Beams Widget";
  gzgt->idname = "NODE_GGT_sbeam";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = WIDGETGROUP_node_sbeam_poll;
  gzgt->setup = WIDGETGROUP_node_sbeam_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->draw_prepare = WIDGETGROUP_node_sbeam_draw_prepare;
  gzgt->refresh = WIDGETGROUP_node_sbeam_refresh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Corner Pin
 * \{ */

struct NodeCornerPinWidgetGroup {
  wmGizmo *gizmos[4];

  struct {
    float2 dims;
    float2 offset;
  } state;
};

static bool WIDGETGROUP_node_corner_pin_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);

  if ((snode->flag & SNODE_BACKDRAW) == 0) {
    return false;
  }

  if (snode && snode->edittree && snode->edittree->type == NTREE_COMPOSIT) {
    bNode *node = bke::node_get_active(*snode->edittree);

    if (node && ELEM(node->type_legacy, CMP_NODE_CORNERPIN)) {
      return true;
    }
  }

  return false;
}

static void WIDGETGROUP_node_corner_pin_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  NodeCornerPinWidgetGroup *cpin_group = (NodeCornerPinWidgetGroup *)MEM_mallocN(
      sizeof(NodeCornerPinWidgetGroup), __func__);
  const wmGizmoType *gzt_move_3d = WM_gizmotype_find("GIZMO_GT_move_3d", false);

  for (int i = 0; i < 4; i++) {
    cpin_group->gizmos[i] = WM_gizmo_new_ptr(gzt_move_3d, gzgroup, nullptr);
    wmGizmo *gz = cpin_group->gizmos[i];

    RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_MOVE_STYLE_CROSS_2D);

    gz->scale_basis = 0.05f / 75.0;
  }

  gzgroup->customdata = cpin_group;
}

static void WIDGETGROUP_node_corner_pin_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  NodeCornerPinWidgetGroup *cpin_group = (NodeCornerPinWidgetGroup *)gzgroup->customdata;
  ARegion *region = CTX_wm_region(C);

  SpaceNode *snode = CTX_wm_space_node(C);

  float matrix_space[4][4];
  node_gizmo_calc_matrix_space_with_image_dims(
      snode, region, cpin_group->state.dims, cpin_group->state.offset, matrix_space);

  for (int i = 0; i < 4; i++) {
    wmGizmo *gz = cpin_group->gizmos[i];
    copy_m4_m4(gz->matrix_space, matrix_space);
  }
}

static void WIDGETGROUP_node_corner_pin_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  Main *bmain = CTX_data_main(C);
  NodeCornerPinWidgetGroup *cpin_group = (NodeCornerPinWidgetGroup *)gzgroup->customdata;

  void *lock;
  Image *ima = BKE_image_ensure_viewer(bmain, IMA_TYPE_COMPOSITE, "Viewer Node");
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, nullptr, &lock);

  if (ibuf) {
    cpin_group->state.dims[0] = (ibuf->x > 0) ? ibuf->x : 64.0f;
    cpin_group->state.dims[1] = (ibuf->y > 0) ? ibuf->y : 64.0f;
    copy_v2_v2(cpin_group->state.offset, ima->runtime.backdrop_offset);

    SpaceNode *snode = CTX_wm_space_node(C);
    bNode *node = bke::node_get_active(*snode->edittree);

    /* need to set property here for undo. TODO: would prefer to do this in _init. */
    int i = 0;
    for (bNodeSocket *sock = (bNodeSocket *)node->inputs.first; sock && i < 4; sock = sock->next) {
      if (sock->type == SOCK_VECTOR) {
        wmGizmo *gz = cpin_group->gizmos[i++];

        PointerRNA sockptr = RNA_pointer_create_discrete(
            (ID *)snode->edittree, &RNA_NodeSocket, sock);
        WM_gizmo_target_property_def_rna(gz, "offset", &sockptr, "default_value", -1);

        WM_gizmo_set_flag(gz, WM_GIZMO_DRAW_MODAL, true);
      }
    }
  }
  else {
    for (int i = 0; i < 4; i++) {
      wmGizmo *gz = cpin_group->gizmos[i];
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
    }
  }

  BKE_image_release_ibuf(ima, ibuf, lock);
}

void NODE_GGT_backdrop_corner_pin(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Corner Pin Widget";
  gzgt->idname = "NODE_GGT_backdrop_corner_pin";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = WIDGETGROUP_node_corner_pin_poll;
  gzgt->setup = WIDGETGROUP_node_corner_pin_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->draw_prepare = WIDGETGROUP_node_corner_pin_draw_prepare;
  gzgt->refresh = WIDGETGROUP_node_corner_pin_refresh;
}

/** \} */

}  // namespace blender::ed::space_node
