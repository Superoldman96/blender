/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/workbench_effect_antialiasing_info.hh"

VERTEX_SHADER_CREATE_INFO(workbench_smaa)

#include "gpu_shader_smaa_lib.glsl"

void main()
{
  int v = gl_VertexID % 3;
  float x = -1.0 + float((v & 1) << 2);
  float y = -1.0 + float((v & 2) << 1);
  gl_Position = vec4(x, y, 1.0, 1.0);
  uvs = (gl_Position.xy + 1.0) * 0.5;

#if SMAA_STAGE == 0
  SMAAEdgeDetectionVS(uvs, offset);
#elif SMAA_STAGE == 1
  SMAABlendingWeightCalculationVS(uvs, pixcoord, offset);
#elif SMAA_STAGE == 2
  SMAANeighborhoodBlendingVS(uvs, offset[0]);
#endif
}
