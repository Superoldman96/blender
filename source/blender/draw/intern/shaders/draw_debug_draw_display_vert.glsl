/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Display debug edge list.
 */

#include "draw_debug_info.hh"

VERTEX_SHADER_CREATE_INFO(draw_debug_draw_display)

void main()
{
  int line_id = (gl_VertexID / 2);
  bool is_provoking_vertex = (gl_VertexID & 1) == 0;
  /* Skip the first vertex containing header data. */
  DRWDebugVertPair vert = in_debug_lines_buf[line_id + drw_debug_draw_offset];

  vec3 pos = uintBitsToFloat((is_provoking_vertex) ? uvec3(vert.pos1_x, vert.pos1_y, vert.pos1_z) :
                                                     uvec3(vert.pos2_x, vert.pos2_y, vert.pos2_z));
  vec4 col = vec4((uvec4(vert.vert_color) >> uvec4(0, 8, 16, 24)) & 0xFFu) / 255.0;

  /* Lifetime management. */
  if (is_provoking_vertex && vert.lifetime > 1) {
    uint vertid = atomicAdd(drw_debug_draw_v_count(out_debug_lines_buf), 2u);
    if (vertid < DRW_DEBUG_DRAW_VERT_MAX) {
      uint out_line_id = vertid / 2u;
      vert.lifetime -= 1;
      out_debug_lines_buf[out_line_id + drw_debug_draw_offset] = vert;
    }
  }

  final_color = col;
  gl_Position = persmat * vec4(pos, 1.0);

  edge_start = edge_pos = (0.5 * (gl_Position.xy / gl_Position.w) + 0.5) * size_viewport;
}
