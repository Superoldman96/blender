/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* GPU module. */
#include "gpu_clip_planes_info.hh"
#include "gpu_index_load_info.hh"
#include "gpu_shader_2D_area_borders_info.hh"
#include "gpu_shader_2D_checker_info.hh"
#include "gpu_shader_2D_diag_stripes_info.hh"
#include "gpu_shader_2D_image_desaturate_color_info.hh"
#include "gpu_shader_2D_image_info.hh"
#include "gpu_shader_2D_image_overlays_merge_info.hh"
#include "gpu_shader_2D_image_overlays_stereo_merge_info.hh"
#include "gpu_shader_2D_image_rect_color_info.hh"
#include "gpu_shader_2D_image_shuffle_color_info.hh"
#include "gpu_shader_2D_node_socket_info.hh"
#include "gpu_shader_2D_nodelink_info.hh"
#include "gpu_shader_2D_point_uniform_size_uniform_color_aa_info.hh"
#include "gpu_shader_2D_point_uniform_size_uniform_color_outline_aa_info.hh"
#include "gpu_shader_2D_point_varying_size_varying_color_info.hh"
#include "gpu_shader_2D_widget_info.hh"
#include "gpu_shader_3D_depth_only_info.hh"
#include "gpu_shader_3D_flat_color_info.hh"
#include "gpu_shader_3D_image_info.hh"
#include "gpu_shader_3D_point_info.hh"
#include "gpu_shader_3D_polyline_info.hh"
#include "gpu_shader_3D_smooth_color_info.hh"
#include "gpu_shader_3D_uniform_color_info.hh"
#include "gpu_shader_gpencil_stroke_info.hh"
#include "gpu_shader_icon_info.hh"
#include "gpu_shader_index_info.hh"
#include "gpu_shader_keyframe_shape_info.hh"
#include "gpu_shader_line_dashed_uniform_color_info.hh"
#include "gpu_shader_print_info.hh"
#include "gpu_shader_sequencer_info.hh"
#include "gpu_shader_simple_lighting_info.hh"
#include "gpu_shader_text_info.hh"
#include "gpu_srgb_to_framebuffer_space_info.hh"

#ifdef WITH_GTEST
#  ifdef WITH_GPU_DRAW_TESTS
#    include "gpu_shader_test_info.hh"
#  endif
#endif

#ifdef WITH_METAL_BACKEND
/* Metal */
#  include "depth_2d_update_info.hh"
#  include "gpu_shader_fullscreen_blit_info.hh"
#endif

/* Realtime compositor. */
#include "compositor_alpha_crop_info.hh"
#include "compositor_bilateral_blur_info.hh"
#include "compositor_bokeh_blur_info.hh"
#include "compositor_bokeh_blur_variable_size_info.hh"
#include "compositor_bokeh_image_info.hh"
#include "compositor_box_mask_info.hh"
#include "compositor_compute_preview_info.hh"
#include "compositor_convert_info.hh"
#include "compositor_cryptomatte_info.hh"
#include "compositor_defocus_info.hh"
#include "compositor_deriche_gaussian_blur_info.hh"
#include "compositor_despeckle_info.hh"
#include "compositor_directional_blur_info.hh"
#include "compositor_displace_info.hh"
#include "compositor_double_edge_mask_info.hh"
#include "compositor_edge_filter_info.hh"
#include "compositor_ellipse_mask_info.hh"
#include "compositor_filter_info.hh"
#include "compositor_flip_info.hh"
#include "compositor_glare_info.hh"
#include "compositor_id_mask_info.hh"
#include "compositor_image_crop_info.hh"
#include "compositor_inpaint_info.hh"
#include "compositor_jump_flooding_info.hh"
#include "compositor_keying_info.hh"
#include "compositor_keying_screen_info.hh"
#include "compositor_kuwahara_info.hh"
#include "compositor_map_uv_info.hh"
#include "compositor_morphological_blur_info.hh"
#include "compositor_morphological_distance_feather_info.hh"
#include "compositor_morphological_distance_info.hh"
#include "compositor_morphological_distance_threshold_info.hh"
#include "compositor_morphological_step_info.hh"
#include "compositor_motion_blur_info.hh"
#include "compositor_movie_distortion_info.hh"
#include "compositor_normalize_info.hh"
#include "compositor_parallel_reduction_info.hh"
#include "compositor_pixelate_info.hh"
#include "compositor_plane_deform_info.hh"
#include "compositor_premultiply_alpha_info.hh"
#include "compositor_projector_lens_distortion_info.hh"
#include "compositor_read_input_info.hh"
#include "compositor_realize_on_domain_info.hh"
#include "compositor_scale_variable_info.hh"
#include "compositor_screen_lens_distortion_info.hh"
#include "compositor_smaa_info.hh"
#include "compositor_split_info.hh"
#include "compositor_summed_area_table_info.hh"
#include "compositor_sun_beams_info.hh"
#include "compositor_symmetric_blur_info.hh"
#include "compositor_symmetric_blur_variable_size_info.hh"
#include "compositor_symmetric_separable_blur_info.hh"
#include "compositor_symmetric_separable_blur_variable_size_info.hh"
#include "compositor_tone_map_photoreceptor_info.hh"
#include "compositor_tone_map_simple_info.hh"
#include "compositor_van_vliet_gaussian_blur_info.hh"
#include "compositor_write_output_info.hh"
#include "compositor_z_combine_info.hh"

/* DRW module. */
#include "draw_debug_info.hh"
#include "draw_fullscreen_info.hh"
#include "draw_hair_refine_info.hh"
#include "draw_object_infos_info.hh"
#include "draw_view_info.hh"

/* Basic engine. */
#include "basic_depth_info.hh"

/* EEVEE engine. */
#include "eevee_ambient_occlusion_info.hh"
#include "eevee_deferred_info.hh"
#include "eevee_depth_of_field_info.hh"
#include "eevee_film_info.hh"
#include "eevee_hiz_info.hh"
#include "eevee_light_culling_info.hh"
#include "eevee_lightprobe_sphere_info.hh"
#include "eevee_lightprobe_volume_info.hh"
#include "eevee_lookdev_info.hh"
#include "eevee_lut_info.hh"
#include "eevee_material_info.hh"
#include "eevee_motion_blur_info.hh"
#include "eevee_shadow_info.hh"
#include "eevee_subsurface_info.hh"
#include "eevee_tracing_info.hh"
#include "eevee_velocity_info.hh"
#include "eevee_volume_info.hh"

/* Image engine. */
#include "engine_image_info.hh"

/* Grease Pencil engine. */
#include "gpencil_info.hh"
#include "gpencil_vfx_info.hh"

/* Overlay engine. */
#include "overlay_antialiasing_info.hh"
#include "overlay_armature_info.hh"
#include "overlay_background_info.hh"
#include "overlay_edit_mode_info.hh"
#include "overlay_extra_info.hh"
#include "overlay_facing_info.hh"
#include "overlay_grid_info.hh"
#include "overlay_outline_info.hh"
#include "overlay_paint_info.hh"
#include "overlay_sculpt_curves_info.hh"
#include "overlay_sculpt_info.hh"
#include "overlay_viewer_attribute_info.hh"
#include "overlay_volume_info.hh"
#include "overlay_wireframe_info.hh"

/* Selection engine. */
#include "select_id_info.hh"

/* Workbench engine. */
#include "workbench_composite_info.hh"
#include "workbench_depth_info.hh"
#include "workbench_effect_antialiasing_info.hh"
#include "workbench_effect_dof_info.hh"
#include "workbench_effect_outline_info.hh"
#include "workbench_prepass_info.hh"
#include "workbench_shadow_info.hh"
#include "workbench_transparent_resolve_info.hh"
#include "workbench_volume_info.hh"
