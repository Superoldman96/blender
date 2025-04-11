/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_armature_info.hh"

VERTEX_SHADER_CREATE_INFO(overlay_armature_wire)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "select_lib.glsl"

void main()
{
  select_id_set(in_select_buf[gl_VertexID / 2]);

  finalColor.rgb = data_buf[gl_VertexID].color_.rgb;
  finalColor.a = 1.0f;

  vec3 world_pos = data_buf[gl_VertexID].pos_.xyz;
  gl_Position = drw_point_world_to_homogenous(world_pos);

  edgeStart = edgePos = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) * sizeViewport;

  view_clipping_distances(world_pos);
}
