/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Virtual Shadow map output.
 *
 * Meshes are rasterize onto an empty frame-buffer. Each generated fragment then checks which
 * virtual page it is supposed to go and load the physical page address.
 * If a physical page exists, we then use atomicMin to mimic a less-than depth test and write to
 * the destination texel.
 */

#include "infos/eevee_material_info.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_geom_mesh)
FRAGMENT_SHADER_CREATE_INFO(eevee_surf_shadow_atomic)

#ifdef GLSL_CPP_STUBS
#  define MAT_SHADOW
#endif

#include "draw_view_lib.glsl"
#include "eevee_nodetree_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_shadow_tilemap_lib.glsl"
#include "eevee_surf_lib.glsl"

vec4 closure_to_rgba(Closure cl)
{
  return vec4(0.0f);
}

void main()
{
  float linear_depth = length(shadow_clip.position);

#ifdef SHADOW_UPDATE_TBDR
  float ndc_depth = gl_FragCoord.z;
/* We need to write to `gl_FragDepth` un-conditionally. So we cannot early exit or use discard. */
#  define discard_result \
    linear_depth = FLT_MAX; \
    ndc_depth = 1.0f;
#else
#  define discard_result \
    discard; \
    return;
#endif

  /* Clip to light shape. */
  if (length_squared(shadow_clip.vector) < 1.0f) {
    discard_result;
  }

#ifdef MAT_TRANSPARENT
  init_globals();

  nodetree_surface(0.0f);

  float noise_offset = sampling_rng_1D_get(SAMPLING_TRANSPARENCY);
  float random_threshold = pcg4d(vec4(g_data.P, noise_offset)).x;

  float transparency = average(g_transmittance);
  if (transparency > random_threshold) {
    discard_result;
  }
#endif

#ifdef SHADOW_UPDATE_ATOMIC_RASTER
  ivec2 texel_co = ivec2(gl_FragCoord.xy);

  /* Using bitwise ops is way faster than integer ops. */
  const int page_shift = SHADOW_PAGE_LOD;
  const int page_mask = ~(0xFFFFFFFF << SHADOW_PAGE_LOD);

  ivec2 tile_co = texel_co >> page_shift;
  ivec2 texel_page = texel_co & page_mask;

  int view_index = shadow_view_id_get();

  int render_page_index = shadow_render_page_index_get(view_index, tile_co);
  uint page_packed = render_map_buf[render_page_index];

  ivec3 page = ivec3(shadow_page_unpack(page_packed));
  /* If the page index is invalid this page shouldn't be rendered,
   * however shadow_page_unpack clamps the result to a valid page.
   * Instead of doing an early return (and introducing branching),
   * we simply ensure the page layer is out-of-bounds. */
  page.z = page_packed < SHADOW_MAX_PAGE ? page.z : -1;

  ivec3 out_texel = ivec3((page.xy << page_shift) | texel_page, page.z);

  uint u_depth = floatBitsToUint(linear_depth);
  imageAtomicMin(shadow_atlas_img, out_texel, u_depth);
#endif

#ifdef SHADOW_UPDATE_TBDR
  gl_FragDepth = ndc_depth;
  out_depth = linear_depth;
#endif
}
