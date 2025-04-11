/* SPDX-FileCopyrightText: 2021-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_info.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_edit_uv_mask_image)

#include "common_colormanagement_lib.glsl"

void main()
{
  vec2 uvs_clamped = clamp(uvs, 0.0f, 1.0f);
  float mask_value = texture_read_as_linearrgb(imgTexture, true, uvs_clamped).r;
  mask_value = mix(1.0f, mask_value, opacity);
  fragColor = vec4(color.rgb * mask_value, color.a);
}
