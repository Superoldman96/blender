/* SPDX-FileCopyrightText: 2019-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_wireframe_info.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_wireframe_base)

#include "gpu_shader_utildefines_lib.glsl"
#include "overlay_common_lib.glsl"
#include "select_lib.glsl"

void main()
{
#if !defined(POINTS) && !defined(CURVES)
  /* Needed only because of wireframe slider.
   * If we could get rid of it would be nice because of performance drain of discard. */
  if (edgeStart.r == -1.0f) {
    discard;
    return;
  }
#endif

  lineOutput = vec4(0.0f);

#if defined(POINTS)
  vec2 centered = abs(gl_PointCoord - vec2(0.5f));
  float dist = max(centered.x, centered.y);

  float fac = dist * dist * 4.0f;
  /* Create a small gradient so that dense objects have a small fresnel effect. */
  /* Non linear blend. */
  vec3 rim_col = sqrt(finalColorInner.rgb);
  vec3 wire_col = sqrt(finalColor.rgb);
  vec3 final_front_col = mix(rim_col, wire_col, 0.35f);
  fragColor = vec4(mix(final_front_col, rim_col, saturate(fac)), 1.0f);
  fragColor *= fragColor;

#elif !defined(SELECT_ENABLE)
  lineOutput = pack_line_data(gl_FragCoord.xy, edgeStart, edgePos);
  fragColor = finalColor;

#  if !defined(CURVES)
  if (use_custom_depth_bias) {
    vec2 dir = lineOutput.xy * 2.0f - 1.0f;
    bool dir_horiz = abs(dir.x) > abs(dir.y);

    vec2 uv = gl_FragCoord.xy * sizeViewportInv;
    float depth_occluder = texture(depthTex, uv).r;
    float depth_min = depth_occluder;
    vec2 uv_offset = sizeViewportInv;
    if (dir_horiz) {
      uv_offset.y = 0.0f;
    }
    else {
      uv_offset.x = 0.0f;
    }

    depth_min = min(depth_min, texture(depthTex, uv - uv_offset).r);
    depth_min = min(depth_min, texture(depthTex, uv + uv_offset).r);

    float delta = abs(depth_occluder - depth_min);

#    ifndef SELECT_ENABLE
    if (gl_FragCoord.z < (depth_occluder + delta) && gl_FragCoord.z > depth_occluder) {
      gl_FragDepth = depth_occluder;
    }
    else {
      gl_FragDepth = gl_FragCoord.z;
    }
#    endif
  }
#  endif
#endif

  select_id_output(select_id);
}
