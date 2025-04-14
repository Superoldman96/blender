/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_color_utils.glsl"

void combine_color_rgb(float r, float g, float b, out float4 col)
{
  col = float4(r, g, b, 1.0f);
}

void combine_color_hsv(float h, float s, float v, out float4 col)
{
  hsv_to_rgb(float4(h, s, v, 1.0f), col);
}

void combine_color_hsl(float h, float s, float l, out float4 col)
{
  hsl_to_rgb(float4(h, s, l, 1.0f), col);
}
