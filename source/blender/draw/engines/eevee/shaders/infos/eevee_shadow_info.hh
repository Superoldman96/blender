/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_glsl_cpp_stubs.hh"

#  include "draw_fullscreen_info.hh"
#  include "draw_object_infos_info.hh"
#  include "draw_view_info.hh"
#  include "eevee_common_info.hh"
#  include "eevee_shader_shared.hh"

#  define SPHERE_PROBE
#endif

#include "draw_defines.hh"
#include "eevee_defines.hh"

#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Shadow pipeline
 * \{ */

GPU_SHADER_CREATE_INFO(eevee_shadow_clipmap_clear)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(SHADOW_CLIPMAP_GROUP_SIZE)
STORAGE_BUF(0, WRITE, ShadowTileMapClip, tilemaps_clip_buf[])
PUSH_CONSTANT(INT, tilemaps_clip_buf_len)
ADDITIONAL_INFO(eevee_shared)
COMPUTE_SOURCE("eevee_shadow_clipmap_clear_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_tilemap_bounds)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(SHADOW_BOUNDS_GROUP_SIZE)
STORAGE_BUF(LIGHT_BUF_SLOT, READ_WRITE, LightData, light_buf[])
STORAGE_BUF(LIGHT_CULL_BUF_SLOT, READ, LightCullingData, light_cull_buf)
STORAGE_BUF(4, READ, uint, casters_id_buf[])
STORAGE_BUF(5, READ_WRITE, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(6, READ, ObjectBounds, bounds_buf[])
STORAGE_BUF(7, READ_WRITE, ShadowTileMapClip, tilemaps_clip_buf[])
PUSH_CONSTANT(INT, resource_len)
TYPEDEF_SOURCE("draw_shader_shared.hh")
ADDITIONAL_INFO(eevee_shared)
COMPUTE_SOURCE("eevee_shadow_tilemap_bounds_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_tilemap_init)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(SHADOW_TILEMAP_RES, SHADOW_TILEMAP_RES)
STORAGE_BUF(0, READ_WRITE, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(1, READ_WRITE, uint, tiles_buf[])
STORAGE_BUF(2, READ_WRITE, ShadowTileMapClip, tilemaps_clip_buf[])
STORAGE_BUF(4, READ_WRITE, uvec2, pages_cached_buf[])
ADDITIONAL_INFO(eevee_shared)
COMPUTE_SOURCE("eevee_shadow_tilemap_init_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_tag_update)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(1, 1, 1)
STORAGE_BUF(0, READ_WRITE, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(1, READ_WRITE, uint, tiles_buf[])
STORAGE_BUF(5, READ, ObjectBounds, bounds_buf[])
STORAGE_BUF(6, READ, uint, resource_ids_buf[])
ADDITIONAL_INFO(eevee_shared)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_view_culling)
COMPUTE_SOURCE("eevee_shadow_tag_update_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_tag_usage_opaque)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(SHADOW_DEPTH_SCAN_GROUP_SIZE, SHADOW_DEPTH_SCAN_GROUP_SIZE)
STORAGE_BUF(5, READ_WRITE, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(6, READ_WRITE, uint, tiles_buf[])
PUSH_CONSTANT(IVEC2, input_depth_extent)
ADDITIONAL_INFO(eevee_shared)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_view_culling)
ADDITIONAL_INFO(eevee_hiz_data)
ADDITIONAL_INFO(eevee_light_data)
COMPUTE_SOURCE("eevee_shadow_tag_usage_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_tag_usage_surfels)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(SURFEL_GROUP_SIZE)
STORAGE_BUF(6, READ_WRITE, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(7, READ_WRITE, uint, tiles_buf[])
PUSH_CONSTANT(INT, directional_level)
ADDITIONAL_INFO(eevee_shared)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_view_culling)
ADDITIONAL_INFO(eevee_light_data)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_surfel_common)
COMPUTE_SOURCE("eevee_shadow_tag_usage_surfels_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_shadow_tag_transparent_iface, interp)
SMOOTH(VEC3, P)
SMOOTH(VEC3, vP)
GPU_SHADER_NAMED_INTERFACE_END(interp)
GPU_SHADER_NAMED_INTERFACE_INFO(eevee_shadow_tag_transparent_flat_iface, interp_flat)
FLAT(VEC3, ls_aabb_min)
FLAT(VEC3, ls_aabb_max)
GPU_SHADER_NAMED_INTERFACE_END(interp_flat)

GPU_SHADER_CREATE_INFO(eevee_shadow_tag_usage_transparent)
DO_STATIC_COMPILATION()
VERTEX_IN(0, VEC3, pos)
STORAGE_BUF(4, READ, ObjectBounds, bounds_buf[])
STORAGE_BUF(5, READ_WRITE, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(6, READ_WRITE, uint, tiles_buf[])
PUSH_CONSTANT(IVEC2, fb_resolution)
PUSH_CONSTANT(INT, fb_lod)
VERTEX_OUT(eevee_shadow_tag_transparent_iface)
VERTEX_OUT(eevee_shadow_tag_transparent_flat_iface)
ADDITIONAL_INFO(eevee_shared)
ADDITIONAL_INFO(draw_resource_id_varying)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_view_culling)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(eevee_hiz_data)
ADDITIONAL_INFO(eevee_light_data)
VERTEX_SOURCE("eevee_shadow_tag_usage_vert.glsl")
FRAGMENT_SOURCE("eevee_shadow_tag_usage_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_tag_usage_volume)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(VOLUME_GROUP_SIZE, VOLUME_GROUP_SIZE, VOLUME_GROUP_SIZE)
STORAGE_BUF(4, READ_WRITE, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(5, READ_WRITE, uint, tiles_buf[])
ADDITIONAL_INFO(eevee_volume_properties_data)
ADDITIONAL_INFO(eevee_shared)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_view_culling)
ADDITIONAL_INFO(eevee_hiz_data)
ADDITIONAL_INFO(eevee_light_data)
ADDITIONAL_INFO(eevee_sampling_data)
COMPUTE_SOURCE("eevee_shadow_tag_usage_volume_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_page_mask)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(SHADOW_TILEMAP_RES, SHADOW_TILEMAP_RES)
PUSH_CONSTANT(INT, max_view_per_tilemap)
STORAGE_BUF(0, READ_WRITE, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(1, READ_WRITE, uint, tiles_buf[])
ADDITIONAL_INFO(eevee_shared)
COMPUTE_SOURCE("eevee_shadow_page_mask_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_page_free)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(SHADOW_TILEMAP_LOD0_LEN)
STORAGE_BUF(0, READ_WRITE, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(1, READ_WRITE, uint, tiles_buf[])
STORAGE_BUF(2, READ_WRITE, ShadowPagesInfoData, pages_infos_buf)
STORAGE_BUF(3, READ_WRITE, uint, pages_free_buf[])
STORAGE_BUF(4, READ_WRITE, uvec2, pages_cached_buf[])
ADDITIONAL_INFO(eevee_shared)
COMPUTE_SOURCE("eevee_shadow_page_free_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_page_defrag)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(1)
TYPEDEF_SOURCE("draw_shader_shared.hh")
STORAGE_BUF(1, READ_WRITE, uint, tiles_buf[])
STORAGE_BUF(2, READ_WRITE, ShadowPagesInfoData, pages_infos_buf)
STORAGE_BUF(3, READ_WRITE, uint, pages_free_buf[])
STORAGE_BUF(4, READ_WRITE, uvec2, pages_cached_buf[])
STORAGE_BUF(5, WRITE, DispatchCommand, clear_dispatch_buf)
STORAGE_BUF(6, WRITE, DrawCommand, tile_draw_buf)
STORAGE_BUF(7, READ_WRITE, ShadowStatistics, statistics_buf)
ADDITIONAL_INFO(eevee_shared)
COMPUTE_SOURCE("eevee_shadow_page_defrag_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_page_allocate)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(SHADOW_TILEMAP_LOD0_LEN)
TYPEDEF_SOURCE("draw_shader_shared.hh")
STORAGE_BUF(0, READ_WRITE, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(1, READ_WRITE, uint, tiles_buf[])
STORAGE_BUF(2, READ_WRITE, ShadowPagesInfoData, pages_infos_buf)
STORAGE_BUF(3, READ_WRITE, uint, pages_free_buf[])
STORAGE_BUF(4, READ_WRITE, uvec2, pages_cached_buf[])
STORAGE_BUF(6, READ_WRITE, ShadowStatistics, statistics_buf)
ADDITIONAL_INFO(eevee_shared)
COMPUTE_SOURCE("eevee_shadow_page_allocate_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_tilemap_finalize)
DO_STATIC_COMPILATION()
TYPEDEF_SOURCE("draw_shader_shared.hh")
LOCAL_GROUP_SIZE(SHADOW_TILEMAP_RES, SHADOW_TILEMAP_RES)
STORAGE_BUF(0, READ, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(1, READ, uint, tiles_buf[])
STORAGE_BUF(2, READ_WRITE, ShadowPagesInfoData, pages_infos_buf)
STORAGE_BUF(3, READ_WRITE, ShadowStatistics, statistics_buf)
STORAGE_BUF(4, WRITE, ViewMatrices, view_infos_buf[SHADOW_VIEW_MAX])
STORAGE_BUF(5, WRITE, ShadowRenderView, render_view_buf[SHADOW_VIEW_MAX])
STORAGE_BUF(6, READ, ShadowTileMapClip, tilemaps_clip_buf[])
IMAGE(0, GPU_R32UI, WRITE, UINT_2D, tilemaps_img)
ADDITIONAL_INFO(eevee_shared)
COMPUTE_SOURCE("eevee_shadow_tilemap_finalize_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_tilemap_rendermap)
DO_STATIC_COMPILATION()
TYPEDEF_SOURCE("draw_shader_shared.hh")
LOCAL_GROUP_SIZE(SHADOW_TILEMAP_RES, SHADOW_TILEMAP_RES)
STORAGE_BUF(0, READ_WRITE, ShadowStatistics, statistics_buf)
STORAGE_BUF(1, READ, ShadowRenderView, render_view_buf[SHADOW_VIEW_MAX])
STORAGE_BUF(2, READ_WRITE, uint, tiles_buf[])
STORAGE_BUF(3, READ_WRITE, DispatchCommand, clear_dispatch_buf)
STORAGE_BUF(4, READ_WRITE, DrawCommand, tile_draw_buf)
STORAGE_BUF(5, WRITE, uint, dst_coord_buf[SHADOW_RENDER_MAP_SIZE])
STORAGE_BUF(6, WRITE, uint, src_coord_buf[SHADOW_RENDER_MAP_SIZE])
STORAGE_BUF(7, WRITE, uint, render_map_buf[SHADOW_RENDER_MAP_SIZE])
ADDITIONAL_INFO(eevee_shared)
COMPUTE_SOURCE("eevee_shadow_tilemap_rendermap_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_tilemap_amend)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(SHADOW_TILEMAP_RES, SHADOW_TILEMAP_RES)
IMAGE(0, GPU_R32UI, READ_WRITE, UINT_2D, tilemaps_img)
STORAGE_BUF(LIGHT_CULL_BUF_SLOT, READ, LightCullingData, light_cull_buf)
STORAGE_BUF(LIGHT_BUF_SLOT, READ_WRITE, LightData, light_buf[])
/* The call bind_resources(lights) also uses LIGHT_ZBIN_BUF_SLOT and LIGHT_TILE_BUF_SLOT. */
STORAGE_BUF(4, READ, ShadowTileMapData, tilemaps_buf[])
ADDITIONAL_INFO(eevee_shared)
ADDITIONAL_INFO(draw_view)
COMPUTE_SOURCE("eevee_shadow_tilemap_amend_comp.glsl")
GPU_SHADER_CREATE_END()

/* AtomicMin clear implementation. */
GPU_SHADER_CREATE_INFO(eevee_shadow_page_clear)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(SHADOW_PAGE_CLEAR_GROUP_SIZE, SHADOW_PAGE_CLEAR_GROUP_SIZE)
STORAGE_BUF(2, READ, ShadowPagesInfoData, pages_infos_buf)
STORAGE_BUF(6, READ, uint, dst_coord_buf[SHADOW_RENDER_MAP_SIZE])
ADDITIONAL_INFO(eevee_shared)
COMPUTE_SOURCE("eevee_shadow_page_clear_comp.glsl")
IMAGE(SHADOW_ATLAS_IMG_SLOT, GPU_R32UI, READ_WRITE, UINT_2D_ARRAY_ATOMIC, shadow_atlas_img)
GPU_SHADER_CREATE_END()

/* TBDR clear implementation. */
GPU_SHADER_CREATE_INFO(eevee_shadow_page_tile_clear)
DO_STATIC_COMPILATION()
DEFINE("PASS_CLEAR")
ADDITIONAL_INFO(eevee_shared)
BUILTINS(BuiltinBits::VIEWPORT_INDEX | BuiltinBits::LAYER)
STORAGE_BUF(8, READ, uint, src_coord_buf[SHADOW_RENDER_MAP_SIZE])
VERTEX_SOURCE("eevee_shadow_page_tile_vert.glsl")
FRAGMENT_SOURCE("eevee_shadow_page_tile_frag.glsl")
FRAGMENT_OUT_ROG(0, FLOAT, out_tile_depth, SHADOW_ROG_ID)
GPU_SHADER_CREATE_END()

#ifdef APPLE
/* Metal supports USHORT which saves a bit of performance here. */
#  define PAGE_Z_TYPE USHORT
#else
#  define PAGE_Z_TYPE UINT
#endif

/* Interface for passing precalculated values in accumulation vertex to frag. */
GPU_SHADER_NAMED_INTERFACE_INFO(eevee_shadow_page_tile_store_noperspective_iface,
                                interp_noperspective)
NO_PERSPECTIVE(VEC2, out_texel_xy)
GPU_SHADER_NAMED_INTERFACE_END(interp_noperspective)
GPU_SHADER_NAMED_INTERFACE_INFO(eevee_shadow_page_tile_store_flat_iface, interp_flat)
FLAT(PAGE_Z_TYPE, out_page_z)
GPU_SHADER_NAMED_INTERFACE_END(interp_flat)

#undef PAGE_Z_TYPE

/* 2nd tile pass to store shadow depths in atlas. */
GPU_SHADER_CREATE_INFO(eevee_shadow_page_tile_store)
DO_STATIC_COMPILATION()
DEFINE("PASS_DEPTH_STORE")
ADDITIONAL_INFO(eevee_shared)
BUILTINS(BuiltinBits::VIEWPORT_INDEX | BuiltinBits::LAYER)
STORAGE_BUF(7, READ, uint, dst_coord_buf[SHADOW_RENDER_MAP_SIZE])
STORAGE_BUF(8, READ, uint, src_coord_buf[SHADOW_RENDER_MAP_SIZE])
SUBPASS_IN(0, FLOAT, FLOAT_2D_ARRAY, in_tile_depth, SHADOW_ROG_ID)
IMAGE(SHADOW_ATLAS_IMG_SLOT, GPU_R32UI, READ_WRITE, UINT_2D_ARRAY, shadow_atlas_img)
VERTEX_OUT(eevee_shadow_page_tile_store_noperspective_iface)
VERTEX_OUT(eevee_shadow_page_tile_store_flat_iface)
VERTEX_SOURCE("eevee_shadow_page_tile_vert.glsl")
FRAGMENT_SOURCE("eevee_shadow_page_tile_frag.glsl")
GPU_SHADER_CREATE_END()

/* Custom visibility check pass. */
GPU_SHADER_CREATE_INFO(eevee_shadow_view_visibility)
DO_STATIC_COMPILATION()
TYPEDEF_SOURCE("eevee_defines.hh")
TYPEDEF_SOURCE("eevee_shader_shared.hh")
LOCAL_GROUP_SIZE(DRW_VISIBILITY_GROUP_SIZE)
DEFINE_VALUE("DRW_VIEW_LEN", STRINGIFY(DRW_VIEW_MAX))
STORAGE_BUF(0, READ, ObjectBounds, bounds_buf[])
STORAGE_BUF(1, READ_WRITE, uint, visibility_buf[])
STORAGE_BUF(2, READ, ShadowRenderView, render_view_buf[SHADOW_VIEW_MAX])
PUSH_CONSTANT(INT, resource_len)
PUSH_CONSTANT(INT, view_len)
PUSH_CONSTANT(INT, visibility_word_per_draw)
COMPUTE_SOURCE("eevee_shadow_visibility_comp.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_view_culling)
ADDITIONAL_INFO(draw_object_infos)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug
 * \{ */

GPU_SHADER_CREATE_INFO(eevee_shadow_debug)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(eevee_shared)
STORAGE_BUF(5, READ, ShadowTileMapData, tilemaps_buf[])
STORAGE_BUF(6, READ, uint, tiles_buf[])
FRAGMENT_OUT_DUAL(0, VEC4, out_color_add, SRC_0)
FRAGMENT_OUT_DUAL(0, VEC4, out_color_mul, SRC_1)
PUSH_CONSTANT(INT, debug_mode)
PUSH_CONSTANT(INT, debug_tilemap_index)
DEPTH_WRITE(DepthWrite::ANY)
FRAGMENT_SOURCE("eevee_shadow_debug_frag.glsl")
ADDITIONAL_INFO(draw_fullscreen)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(eevee_hiz_data)
ADDITIONAL_INFO(eevee_light_data)
ADDITIONAL_INFO(eevee_shadow_data)
GPU_SHADER_CREATE_END()

/** \} */
