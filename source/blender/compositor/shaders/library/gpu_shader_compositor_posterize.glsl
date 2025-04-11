/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

void node_composite_posterize(vec4 color, float steps, out vec4 result)
{
  float sanitized_steps = clamp(steps, 2.0f, 1024.0f);
  result = vec4(floor(color.rgb * sanitized_steps) / sanitized_steps, color.a);
}
