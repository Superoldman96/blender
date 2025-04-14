/* SPDX-FileCopyrightText: 2021-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Extract edge data for object mode wire frame. */

#include "subdiv_lib.glsl"

#ifdef GPU_AMD_DRIVER_BYTE_BUG
COMPUTE_SHADER_CREATE_INFO(subdiv_edge_fac_amd_legacy)
#else
COMPUTE_SHADER_CREATE_INFO(subdiv_edge_fac)
#endif

void write_vec4(uint index, float4 edge_facs)
{
#ifdef GPU_AMD_DRIVER_BYTE_BUG
  for (uint i = 0; i < 4; i++) {
    output_edge_fac[index + i] = edge_facs[i];
  }
#else
  /* Use same scaling as in extract_edge_fac_iter_face_mesh. */
  uint a = uint(edge_facs.x * 255);
  uint b = uint(edge_facs.y * 255);
  uint c = uint(edge_facs.z * 255);
  uint d = uint(edge_facs.w * 255);
  uint packed_edge_fac = d << 24 | c << 16 | b << 8 | a;
  output_edge_fac[index] = packed_edge_fac;
#endif
}

/* From extract_mesh_vbo_edge_fac.cc, keep in sync! */
float loop_edge_factor_get(float3 fa_no, float3 fb_no)
{
  float cosine = dot(fa_no, fb_no);

  /* Re-scale to the slider range. */
  float fac = (200 * (cosine - 1.0f)) + 1.0f;

  /* The maximum value (255) is unreachable through the UI. */
  return clamp(fac, 0.0f, 1.0f) * (254.0f / 255.0f);
}

float compute_line_factor(uint corner_index, float3 face_normal)
{
  if (input_edge_draw_flag[corner_index] == 0) {
    return 1.0f;
  }

  int quad_other = input_poly_other_map[corner_index];
  if (quad_other == -1) {
    /* Boundary edge or non-manifold. */
    return 0.0f;
  }

  uint start_corner_index_other = quad_other * 4;
  PosNorLoop pos_nor0 = pos_nor[start_corner_index_other + 0];
  PosNorLoop pos_nor1 = pos_nor[start_corner_index_other + 1];
  PosNorLoop pos_nor2 = pos_nor[start_corner_index_other + 2];
  float3 v0 = subdiv_get_vertex_pos(pos_nor0);
  float3 v1 = subdiv_get_vertex_pos(pos_nor1);
  float3 v2 = subdiv_get_vertex_pos(pos_nor2);
  float3 face_normal_other = normalize(cross(v1 - v0, v2 - v0));

  return loop_edge_factor_get(face_normal, face_normal_other);
}

void main()
{
  /* We execute for each quad. */
  uint quad_index = get_global_invocation_index();
  if (quad_index >= shader_data.total_dispatch_size) {
    return;
  }

  /* The start index of the loop is quad_index * 4. */
  uint start_loop_index = quad_index * 4;

  /* First compute the face normal, we need it to compute the bihedral edge angle. */
  PosNorLoop pos_nor0 = pos_nor[start_loop_index + 0];
  PosNorLoop pos_nor1 = pos_nor[start_loop_index + 1];
  PosNorLoop pos_nor2 = pos_nor[start_loop_index + 2];
  float3 v0 = subdiv_get_vertex_pos(pos_nor0);
  float3 v1 = subdiv_get_vertex_pos(pos_nor1);
  float3 v2 = subdiv_get_vertex_pos(pos_nor2);
  float3 face_normal = normalize(cross(v1 - v0, v2 - v0));

  float4 edge_facs = float4(0.0f);
  for (uint i = 0; i < 4; i++) {
    edge_facs[i] = compute_line_factor(start_loop_index + i, face_normal);
  }

#ifdef GPU_AMD_DRIVER_BYTE_BUG
  write_vec4(start_loop_index, edge_facs);
#else
  /* When packed into bytes, the index is the same as for the quad. */
  write_vec4(quad_index, edge_facs);
#endif
}
