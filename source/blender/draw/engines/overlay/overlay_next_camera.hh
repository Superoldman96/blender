/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BKE_camera.h"

#include "DEG_depsgraph_query.hh"

#include "BKE_movieclip.h"
#include "BKE_tracking.h"

#include "DNA_camera_types.h"

#include "ED_view3d.hh"

#include "draw_manager_text.hh"
#include "overlay_next_empty.hh"
#include "overlay_next_private.hh"

static float camera_offaxis_shiftx_get(const Scene *scene,
                                       const Object *ob,
                                       float corner_x,
                                       bool right_eye)
{
  const Camera *cam = static_cast<const Camera *>(ob->data);
  if (cam->stereo.convergence_mode == CAM_S3D_OFFAXIS) {
    const char *viewnames[2] = {STEREO_LEFT_NAME, STEREO_RIGHT_NAME};
    const float shiftx = BKE_camera_multiview_shift_x(&scene->r, ob, viewnames[right_eye]);
    const float delta_shiftx = shiftx - cam->shiftx;
    const float width = corner_x * 2.0f;
    return delta_shiftx * width;
  }

  return 0.0;
}

namespace blender::draw::overlay {
struct CameraInstanceData : public ExtraInstanceData {
 public:
  float &volume_start = color_[2];
  float &volume_end = color_[3];
  float &depth = color_[3];
  float &focus = color_[3];
  float4x4 &matrix = object_to_world_;
  float &dist_color_id = matrix[0][3];
  float &corner_x = matrix[0][3];
  float &corner_y = matrix[1][3];
  float &center_x = matrix[2][3];
  float &clip_start = matrix[2][3];
  float &mist_start = matrix[2][3];
  float &center_y = matrix[3][3];
  float &clip_end = matrix[3][3];
  float &mist_end = matrix[3][3];

  CameraInstanceData(const CameraInstanceData &data)
      : CameraInstanceData(data.object_to_world_, data.color_)
  {
  }

  CameraInstanceData(const float4x4 &p_matrix, const float4 &color)
      : ExtraInstanceData(p_matrix, color, 1.0f){};
};

class Cameras {
  using CameraInstanceBuf = ShapeInstanceBuf<ExtraInstanceData>;

 private:
  PassSimple ps_ = {"Cameras"};
  struct CallBuffers {
    const SelectionType selection_type_;
    CameraInstanceBuf distances_buf = {selection_type_, "camera_distances_buf"};
    CameraInstanceBuf frame_buf = {selection_type_, "camera_frame_buf"};
    CameraInstanceBuf tria_buf = {selection_type_, "camera_tria_buf"};
    CameraInstanceBuf tria_wire_buf = {selection_type_, "camera_tria_wire_buf"};
    CameraInstanceBuf volume_buf = {selection_type_, "camera_volume_buf"};
    CameraInstanceBuf volume_wire_buf = {selection_type_, "camera_volume_wire_buf"};
    CameraInstanceBuf sphere_solid_buf = {selection_type_, "camera_sphere_solid_buf"};
    LineInstanceBuf stereo_connect_lines = {selection_type_, "camera_dashed_lines_buf"};
    LineInstanceBuf tracking_path = {selection_type_, "camera_tracking_path_buf"};
    Empties::CallBuffers empties{selection_type_};
  } call_buffers_;

  static void view3d_reconstruction(const select::ID select_id,
                                    const Scene *scene,
                                    const View3D *v3d,
                                    const float4 &color,
                                    const ObjectRef &ob_ref,
                                    Resources &res,
                                    CallBuffers &call_buffers)
  {
    const DRWContextState *draw_ctx = DRW_context_state_get();
    const bool is_select = DRW_state_is_select();
    Object *ob = ob_ref.object;

    MovieClip *clip = BKE_object_movieclip_get((Scene *)scene, ob, false);
    if (clip == nullptr) {
      return;
    }

    const bool is_solid_bundle = (v3d->bundle_drawtype == OB_EMPTY_SPHERE) &&
                                 ((v3d->shading.type != OB_SOLID) || !XRAY_FLAG_ENABLED(v3d));

    MovieTracking *tracking = &clip->tracking;
    /* Index must start in 1, to mimic BKE_tracking_track_get_for_selection_index. */
    int track_index = 1;

    float4 bundle_color_custom;
    float *bundle_color_solid = G_draw.block.color_bundle_solid;
    float *bundle_color_unselected = G_draw.block.color_wire;
    uchar4 text_color_selected, text_color_unselected;
    /* Color Management: Exception here as texts are drawn in sRGB space directly. */
    UI_GetThemeColor4ubv(TH_SELECT, text_color_selected);
    UI_GetThemeColor4ubv(TH_TEXT, text_color_unselected);

    float4x4 camera_mat;
    BKE_tracking_get_camera_object_matrix(ob, camera_mat.ptr());

    const float4x4 object_to_world{ob->object_to_world().ptr()};

    LISTBASE_FOREACH (MovieTrackingObject *, tracking_object, &tracking->objects) {
      float4x4 tracking_object_mat;

      if (tracking_object->flag & TRACKING_OBJECT_CAMERA) {
        tracking_object_mat = camera_mat;
      }
      else {
        const int framenr = BKE_movieclip_remap_scene_to_clip_frame(
            clip, DEG_get_ctime(draw_ctx->depsgraph));

        float4x4 object_mat;
        BKE_tracking_camera_get_reconstructed_interpolate(
            tracking, tracking_object, framenr, object_mat.ptr());

        tracking_object_mat = object_to_world * math::invert(object_mat);
      }

      LISTBASE_FOREACH (MovieTrackingTrack *, track, &tracking_object->tracks) {
        if ((track->flag & TRACK_HAS_BUNDLE) == 0) {
          continue;
        }
        bool is_selected = TRACK_SELECTED(track);

        float4x4 bundle_mat = math::translate(tracking_object_mat, float3{track->bundle_pos});

        const float *bundle_color;
        if (track->flag & TRACK_CUSTOMCOLOR) {
          /* Meh, hardcoded srgb transform here. */
          /* TODO: change the actual DNA color to be linear. */
          srgb_to_linearrgb_v3_v3(bundle_color_custom, track->color);
          bundle_color_custom[3] = 1.0;

          bundle_color = bundle_color_custom;
        }
        else if (is_solid_bundle) {
          bundle_color = bundle_color_solid;
        }
        else if (is_selected) {
          bundle_color = color;
        }
        else {
          bundle_color = bundle_color_unselected;
        }

        const select::ID track_select_id = is_select ? res.select_id(ob_ref, track_index++ << 16) :
                                                       select_id;
        if (is_solid_bundle) {
          if (is_selected) {
            Empties::object_sync(track_select_id,
                                 bundle_mat,
                                 v3d->bundle_size,
                                 v3d->bundle_drawtype,
                                 color,
                                 call_buffers.empties);
          }

          call_buffers.sphere_solid_buf.append(
              ExtraInstanceData{bundle_mat, {float3{bundle_color}, 1.0f}, v3d->bundle_size},
              track_select_id);
        }
        else {
          Empties::object_sync(track_select_id,
                               bundle_mat,
                               v3d->bundle_size,
                               v3d->bundle_drawtype,
                               bundle_color,
                               call_buffers.empties);
        }

        if ((v3d->flag2 & V3D_SHOW_BUNDLENAME) && !is_select) {
          DRWTextStore *dt = DRW_text_cache_ensure();

          DRW_text_cache_add(dt,
                             bundle_mat[3],
                             track->name,
                             strlen(track->name),
                             10,
                             0,
                             DRW_TEXT_CACHE_GLOBALSPACE | DRW_TEXT_CACHE_STRING_PTR,
                             is_selected ? text_color_selected : text_color_unselected);
        }
      }

      if ((v3d->flag2 & V3D_SHOW_CAMERAPATH) && (tracking_object->flag & TRACKING_OBJECT_CAMERA) &&
          !is_select)
      {
        const MovieTrackingReconstruction *reconstruction = &tracking_object->reconstruction;

        if (reconstruction->camnr) {
          const MovieReconstructedCamera *camera = reconstruction->cameras;
          float3 v0, v1;
          for (int a = 0; a < reconstruction->camnr; a++, camera++) {
            v0 = v1;
            v1 = math::transform_point(camera_mat, float3(camera->mat[3]));
            if (a > 0) {
              /* This one is suboptimal (gl_lines instead of gl_line_strip)
               * but we keep this for simplicity */
              call_buffers.tracking_path.append(v0, v1, TH_CAMERA_PATH, select_id);
            }
          }
        }
      }
    }
  }

  /**
   * Draw the stereo 3d support elements (cameras, plane, volume).
   * They are only visible when not looking through the camera:
   */
  static void stereoscopy_extra(const CameraInstanceData &instdata,
                                const select::ID select_id,
                                const Scene *scene,
                                const View3D *v3d,
                                Resources &res,
                                Object *ob,
                                CallBuffers &call_buffers)
  {
    CameraInstanceData stereodata = instdata;

    const Camera *cam = static_cast<const Camera *>(ob->data);
    const bool is_select = DRW_state_is_select();
    const char *viewnames[2] = {STEREO_LEFT_NAME, STEREO_RIGHT_NAME};

    const bool is_stereo3d_cameras = (v3d->stereo3d_flag & V3D_S3D_DISPCAMERAS) != 0;
    const bool is_stereo3d_plane = (v3d->stereo3d_flag & V3D_S3D_DISPPLANE) != 0;
    const bool is_stereo3d_volume = (v3d->stereo3d_flag & V3D_S3D_DISPVOLUME) != 0;

    if (!is_stereo3d_cameras) {
      /* Draw single camera. */
      call_buffers.frame_buf.append(instdata, select_id);
    }

    for (const int eye : IndexRange(2)) {
      ob = BKE_camera_multiview_render(scene, ob, viewnames[eye]);
      BKE_camera_multiview_model_matrix(&scene->r, ob, viewnames[eye], stereodata.matrix.ptr());

      stereodata.corner_x = instdata.corner_x;
      stereodata.corner_y = instdata.corner_y;
      stereodata.center_x = instdata.center_x +
                            camera_offaxis_shiftx_get(scene, ob, instdata.corner_x, eye);
      stereodata.center_y = instdata.center_y;
      stereodata.depth = instdata.depth;

      if (is_stereo3d_cameras) {
        call_buffers.frame_buf.append(stereodata, select_id);

        /* Connecting line between cameras. */
        call_buffers.stereo_connect_lines.append(stereodata.matrix.location(),
                                                 instdata.object_to_world_.location(),
                                                 res.theme_settings.color_wire,
                                                 select_id);
      }

      if (is_stereo3d_volume && !is_select) {
        float r = (eye == 1) ? 2.0f : 1.0f;

        stereodata.volume_start = -cam->clip_start;
        stereodata.volume_end = -cam->clip_end;
        /* Encode eye + intensity and alpha (see shader) */
        copy_v2_fl2(stereodata.color_, r + 0.15f, 1.0f);
        call_buffers.volume_wire_buf.append(stereodata, select_id);

        if (v3d->stereo3d_volume_alpha > 0.0f) {
          /* Encode eye + intensity and alpha (see shader) */
          copy_v2_fl2(stereodata.color_, r + 0.999f, v3d->stereo3d_volume_alpha);
          call_buffers.volume_buf.append(stereodata, select_id);
        }
        /* restore */
        copy_v3_v3(stereodata.color_, instdata.color_);
      }
    }

    if (is_stereo3d_plane && !is_select) {
      if (cam->stereo.convergence_mode == CAM_S3D_TOE) {
        /* There is no real convergence plane but we highlight the center
         * point where the views are pointing at. */
        // zero_v3(stereodata.mat[0]); /* We reconstruct from Z and Y */
        // zero_v3(stereodata.mat[1]); /* Y doesn't change */
        stereodata.matrix.z_axis() = float3(0.0f);
        stereodata.matrix.location() = float3(0.0f);
        for (int i : IndexRange(2)) {
          float4x4 mat;
          /* Need normalized version here. */
          BKE_camera_multiview_model_matrix(&scene->r, ob, viewnames[i], mat.ptr());
          stereodata.matrix.z_axis() += mat.z_axis();
          stereodata.matrix.location() += mat.location() * 0.5f;
        }
        stereodata.matrix.z_axis() = math::normalize(stereodata.matrix.z_axis());
        stereodata.matrix.x_axis() = math::cross(stereodata.matrix.y_axis(),
                                                 stereodata.matrix.z_axis());
      }
      else if (cam->stereo.convergence_mode == CAM_S3D_PARALLEL) {
        /* Show plane at the given distance between the views even if it makes no sense. */
        stereodata.matrix.location() = float3(0.0f);
        for (int i : IndexRange(2)) {
          float4x4 mat;
          BKE_camera_multiview_model_matrix_scaled(&scene->r, ob, viewnames[i], mat.ptr());
          stereodata.matrix.location() += mat.location() * 0.5f;
        }
      }
      else if (cam->stereo.convergence_mode == CAM_S3D_OFFAXIS) {
        /* Nothing to do. Everything is already setup. */
      }
      stereodata.volume_start = -cam->stereo.convergence_distance;
      stereodata.volume_end = -cam->stereo.convergence_distance;
      /* Encode eye + intensity and alpha (see shader) */
      copy_v2_fl2(stereodata.color_, 0.1f, 1.0f);
      call_buffers.volume_wire_buf.append(stereodata, select_id);

      if (v3d->stereo3d_convergence_alpha > 0.0f) {
        /* Encode eye + intensity and alpha (see shader) */
        copy_v2_fl2(stereodata.color_, 0.0f, v3d->stereo3d_convergence_alpha);
        call_buffers.volume_buf.append(stereodata, select_id);
      }
    }
  }

 public:
  Cameras(const SelectionType selection_type) : call_buffers_{selection_type} {};

  void begin_sync()
  {
    call_buffers_.distances_buf.clear();
    call_buffers_.frame_buf.clear();
    call_buffers_.tria_buf.clear();
    call_buffers_.tria_wire_buf.clear();
    call_buffers_.volume_buf.clear();
    call_buffers_.volume_wire_buf.clear();
    call_buffers_.sphere_solid_buf.clear();
    call_buffers_.stereo_connect_lines.clear();
    call_buffers_.tracking_path.clear();
    Empties::begin_sync(call_buffers_.empties);
  }

  void object_sync(const ObjectRef &ob_ref, Resources &res, State &state)
  {
    Object *ob = ob_ref.object;
    const select::ID select_id = res.select_id(ob_ref);
    CameraInstanceData data(ob->object_to_world(), res.object_wire_color(ob_ref, state));

    const View3D *v3d = state.v3d;
    const Scene *scene = state.scene;
    const RegionView3D *rv3d = state.rv3d;

    const Camera *cam = static_cast<Camera *>(ob->data);
    const Object *camera_object = DEG_get_evaluated_object(state.depsgraph, v3d->camera);
    const bool is_select = DRW_state_is_select();
    const bool is_active = (ob == camera_object);
    const bool is_camera_view = (is_active && (rv3d->persp == RV3D_CAMOB));

    const bool is_multiview = (scene->r.scemode & R_MULTIVIEW) != 0;
    const bool is_stereo3d_view = (scene->r.views_format == SCE_VIEWS_FORMAT_STEREO_3D);
    const bool is_stereo3d_display_extra = is_active && is_multiview && (!is_camera_view) &&
                                           ((v3d->stereo3d_flag) != 0);
    const bool is_selection_camera_stereo = is_select && is_camera_view && is_multiview &&
                                            is_stereo3d_view;

    float3 scale = math::to_scale(data.matrix);
    /* BKE_camera_multiview_model_matrix already accounts for scale, don't do it here. */
    if (is_selection_camera_stereo) {
      scale = float3(1.0f);
    }
    else if (ELEM(0.0f, scale.x, scale.y, scale.z)) {
      /* Avoid division by 0. */
      return;
    }
    float4x3 vecs;
    float2 aspect_ratio;
    float2 shift;
    float drawsize;

    BKE_camera_view_frame_ex(scene,
                             cam,
                             cam->drawsize,
                             is_camera_view,
                             1.0f / scale,
                             aspect_ratio,
                             shift,
                             &drawsize,
                             vecs.ptr());

    /* Apply scale to simplify the rest of the drawing. */
    for (int i = 0; i < 4; i++) {
      vecs[i] *= scale;
      /* Project to z=-1 plane. Makes positioning / scaling easier. (see shader) */
      mul_v2_fl(vecs[i], 1.0f / std::abs(vecs[i].z));
    }

    /* Frame coords */
    const float2 center = (vecs[0].xy() + vecs[2].xy()) * 0.5f;
    const float2 corner = vecs[0].xy() - center.xy();
    data.corner_x = corner.x;
    data.corner_y = corner.y;
    data.center_x = center.x;
    data.center_y = center.y;
    data.depth = vecs[0].z;

    if (is_camera_view) {
      /* TODO(Miguel Pozo) */
      if (!DRW_state_is_image_render()) {
        /* Only draw the frame. */
        if (is_multiview) {
          float4x4 mat;
          const bool is_right = v3d->multiview_eye == STEREO_RIGHT_ID;
          const char *view_name = is_right ? STEREO_RIGHT_NAME : STEREO_LEFT_NAME;
          BKE_camera_multiview_model_matrix(&scene->r, ob, view_name, mat.ptr());
          data.center_x += camera_offaxis_shiftx_get(scene, ob, data.corner_x, is_right);
          for (int i : IndexRange(4)) {
            /* Partial copy to avoid overriding packed data. */
            copy_v3_v3(data.matrix[i], mat[i].xyz());
          }
        }
        data.depth *= -1.0f; /* Hides the back of the camera wires (see shader). */
        call_buffers_.frame_buf.append(data, select_id);
      }
    }
    else {
      /* Stereo cameras, volumes, plane drawing. */
      if (is_stereo3d_display_extra) {
        stereoscopy_extra(data, select_id, scene, v3d, res, ob, call_buffers_);
      }
      else {
        call_buffers_.frame_buf.append(data, select_id);
      }
    }

    if (!is_camera_view) {
      /* Triangle. */
      float tria_size = 0.7f * drawsize / fabsf(data.depth);
      float tria_margin = 0.1f * drawsize / fabsf(data.depth);
      data.center_x = center.x;
      data.center_y = center.y + data.corner_y + tria_margin + tria_size;
      data.corner_x = data.corner_y = -tria_size;
      (is_active ? call_buffers_.tria_buf : call_buffers_.tria_wire_buf).append(data, select_id);
    }

    if (cam->flag & CAM_SHOWLIMITS) {
      /* Scale focus point. */
      data.matrix.x_axis() *= cam->drawsize;
      data.matrix.y_axis() *= cam->drawsize;

      data.dist_color_id = (is_active) ? 3 : 2;
      data.focus = -BKE_camera_object_dof_distance(ob);
      data.clip_start = cam->clip_start;
      data.clip_end = cam->clip_end;
      call_buffers_.distances_buf.append(data, select_id);
    }

    if (cam->flag & CAM_SHOWMIST) {
      World *world = scene->world;
      if (world) {
        data.dist_color_id = (is_active) ? 1 : 0;
        data.focus = 1.0f; /* Disable */
        data.mist_start = world->miststa;
        data.mist_end = world->miststa + world->mistdist;
        call_buffers_.distances_buf.append(data, select_id);
      }
    }

    /* Motion Tracking. */
    if ((v3d->flag2 & V3D_SHOW_RECONSTRUCTION) != 0) {
      view3d_reconstruction(
          select_id, scene, v3d, res.object_wire_color(ob_ref, state), ob_ref, res, call_buffers_);
    }

    // TODO: /* Background images. */
    // if (look_through && (cam->flag & CAM_SHOW_BG_IMAGE) &&
    // !BLI_listbase_is_empty(&cam->bg_images))
    // {
    //   OVERLAY_image_camera_cache_populate(vedata, ob);
    // }
  }

  void end_sync(Resources &res, ShapeCache &shapes, const State &state)
  {
    ps_.init();
    res.select_bind(ps_);

    {
      PassSimple::Sub &sub_pass = ps_.sub("volume");
      sub_pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA |
                         DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_CULL_BACK | state.clipping_state);
      sub_pass.shader_set(res.shaders.extra_shape.get());
      sub_pass.bind_ubo("globalsBlock", &res.globals_buf);
      call_buffers_.volume_buf.end_sync(sub_pass, shapes.camera_volume.get());
    }
    {
      PassSimple::Sub &sub_pass = ps_.sub("volume_wire");
      sub_pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA |
                         DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_CULL_BACK | state.clipping_state);
      sub_pass.shader_set(res.shaders.extra_shape.get());
      sub_pass.bind_ubo("globalsBlock", &res.globals_buf);
      call_buffers_.volume_wire_buf.end_sync(sub_pass, shapes.camera_volume_wire.get());
    }

    {
      PassSimple::Sub &sub_pass = ps_.sub("camera_shapes");
      sub_pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH |
                         DRW_STATE_DEPTH_LESS_EQUAL | state.clipping_state);
      sub_pass.shader_set(res.shaders.extra_shape.get());
      sub_pass.bind_ubo("globalsBlock", &res.globals_buf);
      call_buffers_.distances_buf.end_sync(sub_pass, shapes.camera_distances.get());
      call_buffers_.frame_buf.end_sync(sub_pass, shapes.camera_frame.get());
      call_buffers_.tria_buf.end_sync(sub_pass, shapes.camera_tria.get());
      call_buffers_.tria_wire_buf.end_sync(sub_pass, shapes.camera_tria_wire.get());
      call_buffers_.sphere_solid_buf.end_sync(sub_pass, shapes.sphere_low_detail.get());
    }
    {
      PassSimple::Sub &sub_pass = ps_.sub("camera_extra_wire");
      sub_pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH |
                         DRW_STATE_DEPTH_LESS_EQUAL | state.clipping_state);
      sub_pass.shader_set(res.shaders.extra_wire.get());
      sub_pass.bind_ubo("globalsBlock", &res.globals_buf);
      call_buffers_.stereo_connect_lines.end_sync(sub_pass);
      call_buffers_.tracking_path.end_sync(sub_pass);
    }
    {
      PassSimple::Sub &sub_pass = ps_.sub("empties");
      Empties::end_sync(res, shapes, state, sub_pass, call_buffers_.empties);
    }
  }

  void draw(Framebuffer &framebuffer, Manager &manager, View &view)
  {
    GPU_framebuffer_bind(framebuffer);
    manager.submit(ps_, view);
  }
};

}  // namespace blender::draw::overlay
