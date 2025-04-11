/* SPDX-FileCopyrightText: 2019-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_bicubic_sampler_lib.glsl"

void point_texco_remap_square(vec3 vin, out vec3 vout)
{
  vout = vin * 2.0f - 1.0f;
}

void point_texco_clamp(vec3 vin, sampler2D ima, out vec3 vout)
{
  vec2 half_texel_size = 0.5f / vec2(textureSize(ima, 0).xy);
  vout = clamp(vin, half_texel_size.xyy, 1.0f - half_texel_size.xyy);
}

void point_map_to_sphere(vec3 vin, out vec3 vout)
{
  float len = length(vin);
  float v, u;
  if (len > 0.0f) {
    if (vin.x == 0.0f && vin.y == 0.0f) {
      u = 0.0f;
    }
    else {
      u = (1.0f - atan(vin.x, vin.y) / M_PI) / 2.0f;
    }

    v = 1.0f - acos(vin.z / len) / M_PI;
  }
  else {
    v = u = 0.0f;
  }

  vout = vec3(u, v, 0.0f);
}

void point_map_to_tube(vec3 vin, out vec3 vout)
{
  float u, v;
  v = (vin.z + 1.0f) * 0.5f;
  float len = sqrt(vin.x * vin.x + vin.y * vin[1]);
  if (len > 0.0f) {
    u = (1.0f - (atan(vin.x / len, vin.y / len) / M_PI)) * 0.5f;
  }
  else {
    v = u = 0.0f;
  }

  vout = vec3(u, v, 0.0f);
}

/* 16bits floats limits. Higher/Lower values produce +/-inf. */
#define safe_color(a) (clamp(a, -65520.0f, 65520.0f))

void node_tex_image_linear(vec3 co, sampler2D ima, out vec4 color, out float alpha)
{
#ifdef GPU_FRAGMENT_SHADER
  vec2 dx = dFdx(co.xy) * texture_lod_bias_get();
  vec2 dy = dFdy(co.xy) * texture_lod_bias_get();

  color = safe_color(textureGrad(ima, co.xy, dx, dy));
#else
  color = safe_color(texture(ima, co.xy));
#endif

  alpha = color.a;
}

void node_tex_image_cubic(vec3 co, sampler2D ima, out vec4 color, out float alpha)
{
  color = safe_color(texture_bicubic(ima, co.xy));
  alpha = color.a;
}

void tex_box_sample_linear(
    vec3 texco, vec3 N, sampler2D ima, out vec4 color1, out vec4 color2, out vec4 color3)
{
  /* X projection */
  vec2 uv = texco.yz;
  if (N.x < 0.0f) {
    uv.x = 1.0f - uv.x;
  }
  color1 = texture(ima, uv);
  /* Y projection */
  uv = texco.xz;
  if (N.y > 0.0f) {
    uv.x = 1.0f - uv.x;
  }
  color2 = texture(ima, uv);
  /* Z projection */
  uv = texco.yx;
  if (N.z > 0.0f) {
    uv.x = 1.0f - uv.x;
  }
  color3 = texture(ima, uv);
}

void tex_box_sample_cubic(
    vec3 texco, vec3 N, sampler2D ima, out vec4 color1, out vec4 color2, out vec4 color3)
{
  float alpha;
  /* X projection */
  vec2 uv = texco.yz;
  if (N.x < 0.0f) {
    uv.x = 1.0f - uv.x;
  }
  node_tex_image_cubic(uv.xyy, ima, color1, alpha);
  /* Y projection */
  uv = texco.xz;
  if (N.y > 0.0f) {
    uv.x = 1.0f - uv.x;
  }
  node_tex_image_cubic(uv.xyy, ima, color2, alpha);
  /* Z projection */
  uv = texco.yx;
  if (N.z > 0.0f) {
    uv.x = 1.0f - uv.x;
  }
  node_tex_image_cubic(uv.xyy, ima, color3, alpha);
}

void tex_box_blend(
    vec3 N, vec4 color1, vec4 color2, vec4 color3, float blend, out vec4 color, out float alpha)
{
  /* project from direction vector to barycentric coordinates in triangles */
  N = abs(N);
  N /= dot(N, vec3(1.0f));

  /* basic idea is to think of this as a triangle, each corner representing
   * one of the 3 faces of the cube. in the corners we have single textures,
   * in between we blend between two textures, and in the middle we a blend
   * between three textures.
   *
   * the `Nxyz` values are the barycentric coordinates in an equilateral
   * triangle, which in case of blending, in the middle has a smaller
   * equilateral triangle where 3 textures blend. this divides things into
   * 7 zones, with an if () test for each zone
   * EDIT: Now there is only 4 if's. */

  float limit = 0.5f + 0.5f * blend;

  vec3 weight;
  weight = N.xyz / (N.xyx + N.yzz);
  weight = clamp((weight - 0.5f * (1.0f - blend)) / max(1e-8f, blend), 0.0f, 1.0f);

  /* test for mixes between two textures */
  if (N.z < (1.0f - limit) * (N.y + N.x)) {
    weight.z = 0.0f;
    weight.y = 1.0f - weight.x;
  }
  else if (N.x < (1.0f - limit) * (N.y + N.z)) {
    weight.x = 0.0f;
    weight.z = 1.0f - weight.y;
  }
  else if (N.y < (1.0f - limit) * (N.x + N.z)) {
    weight.y = 0.0f;
    weight.x = 1.0f - weight.z;
  }
  else {
    /* last case, we have a mix between three */
    weight = ((2.0f - limit) * N + (limit - 1.0f)) / max(1e-8f, blend);
  }

  color = weight.x * color1 + weight.y * color2 + weight.z * color3;
  alpha = color.a;
}

void node_tex_image_empty(vec3 co, out vec4 color, out float alpha)
{
  color = vec4(0.0f);
  alpha = 0.0f;
}

bool node_tex_tile_lookup(inout vec3 co, sampler2DArray ima, sampler1DArray map)
{
  vec2 tile_pos = floor(co.xy);

  if (tile_pos.x < 0 || tile_pos.y < 0 || tile_pos.x >= 10)
    return false;

  float tile = 10 * tile_pos.y + tile_pos.x;
  if (tile >= textureSize(map, 0).x)
    return false;

  /* Fetch tile information. */
  float tile_layer = texelFetch(map, ivec2(tile, 0), 0).x;
  if (tile_layer < 0)
    return false;

  vec4 tile_info = texelFetch(map, ivec2(tile, 1), 0);

  co = vec3(((co.xy - tile_pos) * tile_info.zw) + tile_info.xy, tile_layer);
  return true;
}

void node_tex_tile_linear(
    vec3 co, sampler2DArray ima, sampler1DArray map, out vec4 color, out float alpha)
{
  if (node_tex_tile_lookup(co, ima, map)) {
    color = safe_color(texture(ima, co));
  }
  else {
    color = vec4(1.0f, 0.0f, 1.0f, 1.0f);
  }

  alpha = color.a;
}

void node_tex_tile_cubic(
    vec3 co, sampler2DArray ima, sampler1DArray map, out vec4 color, out float alpha)
{
  if (node_tex_tile_lookup(co, ima, map)) {
    vec2 tex_size = vec2(textureSize(ima, 0).xy);

    co.xy *= tex_size;
    /* texel center */
    vec2 tc = floor(co.xy - 0.5f) + 0.5f;
    vec2 w0, w1, w2, w3;
    cubic_bspline_coefficients(co.xy - tc, w0, w1, w2, w3);

    vec2 s0 = w0 + w1;
    vec2 s1 = w2 + w3;

    vec2 f0 = w1 / (w0 + w1);
    vec2 f1 = w3 / (w2 + w3);

    vec4 final_co;
    final_co.xy = tc - 1.0f + f0;
    final_co.zw = tc + 1.0f + f1;
    final_co /= tex_size.xyxy;

    color = safe_color(textureLod(ima, vec3(final_co.xy, co.z), 0.0f)) * s0.x * s0.y;
    color += safe_color(textureLod(ima, vec3(final_co.zy, co.z), 0.0f)) * s1.x * s0.y;
    color += safe_color(textureLod(ima, vec3(final_co.xw, co.z), 0.0f)) * s0.x * s1.y;
    color += safe_color(textureLod(ima, vec3(final_co.zw, co.z), 0.0f)) * s1.x * s1.y;
  }
  else {
    color = vec4(1.0f, 0.0f, 1.0f, 1.0f);
  }

  alpha = color.a;
}
