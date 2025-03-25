/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <string>

#include "BKE_global.hh"
#if defined(WIN32)
#  include "BLI_winstuff.h"
#endif
#include "BLI_string_ref.hh"
#include "BLI_subprocess.hh"
#include "BLI_threads.h"
#include "BLI_vector.hh"

#include "DNA_userdef_types.h"

#include "gpu_capabilities_private.hh"
#include "gpu_platform_private.hh"

#include "gl_debug.hh"

#include "gl_backend.hh"

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Platform
 * \{ */

static bool match_renderer(StringRef renderer, const Vector<std::string> &items)
{
  for (const std::string &item : items) {
    const std::string wrapped = " " + item + " ";
    if (renderer.endswith(item) || renderer.find(wrapped) != StringRef::not_found) {
      return true;
    }
  }
  return false;
}

void GLBackend::platform_init()
{
  BLI_assert(!GPG.initialized);

  const char *vendor = (const char *)glGetString(GL_VENDOR);
  const char *renderer = (const char *)glGetString(GL_RENDERER);
  const char *version = (const char *)glGetString(GL_VERSION);
  eGPUDeviceType device = GPU_DEVICE_ANY;
  eGPUOSType os = GPU_OS_ANY;
  eGPUDriverType driver = GPU_DRIVER_ANY;
  eGPUSupportLevel support_level = GPU_SUPPORT_LEVEL_SUPPORTED;

#ifdef _WIN32
  os = GPU_OS_WIN;
#else
  os = GPU_OS_UNIX;
#endif

  if (!vendor) {
    printf("Warning: No OpenGL vendor detected.\n");
    device = GPU_DEVICE_UNKNOWN;
    driver = GPU_DRIVER_ANY;
  }
  else if (strstr(vendor, "ATI") || strstr(vendor, "AMD")) {
    device = GPU_DEVICE_ATI;
    driver = GPU_DRIVER_OFFICIAL;
  }
  else if (strstr(vendor, "NVIDIA")) {
    device = GPU_DEVICE_NVIDIA;
    driver = GPU_DRIVER_OFFICIAL;
  }
  else if (strstr(vendor, "Intel") ||
           /* src/mesa/drivers/dri/intel/intel_context.c */
           strstr(renderer, "Mesa DRI Intel") || strstr(renderer, "Mesa DRI Mobile Intel"))
  {
    device = GPU_DEVICE_INTEL;
    driver = GPU_DRIVER_OFFICIAL;

    if (strstr(renderer, "UHD Graphics") ||
        /* Not UHD but affected by the same bugs. */
        strstr(renderer, "HD Graphics 530") || strstr(renderer, "Kaby Lake GT2") ||
        strstr(renderer, "Whiskey Lake"))
    {
      device |= GPU_DEVICE_INTEL_UHD;
    }
  }
  else if (strstr(renderer, "Mesa DRI R") ||
           (strstr(renderer, "Radeon") && strstr(vendor, "X.Org")) ||
           (strstr(renderer, "AMD") && strstr(vendor, "X.Org")) ||
           (strstr(renderer, "Gallium ") && strstr(renderer, " on ATI ")) ||
           (strstr(renderer, "Gallium ") && strstr(renderer, " on AMD ")))
  {
    device = GPU_DEVICE_ATI;
    driver = GPU_DRIVER_OPENSOURCE;
  }
  else if (strstr(renderer, "Nouveau") || strstr(vendor, "nouveau")) {
    device = GPU_DEVICE_NVIDIA;
    driver = GPU_DRIVER_OPENSOURCE;
  }
  else if (strstr(vendor, "Mesa")) {
    device = GPU_DEVICE_SOFTWARE;
    driver = GPU_DRIVER_SOFTWARE;
  }
  else if (strstr(vendor, "Microsoft")) {
    /* Qualcomm devices use Mesa's GLOn12, which claims to be vended by Microsoft */
    if (strstr(renderer, "Qualcomm")) {
      device = GPU_DEVICE_QUALCOMM;
      driver = GPU_DRIVER_OFFICIAL;
    }
    else {
      device = GPU_DEVICE_SOFTWARE;
      driver = GPU_DRIVER_SOFTWARE;
    }
  }
  else if (strstr(vendor, "Apple")) {
    /* Apple Silicon. */
    device = GPU_DEVICE_APPLE;
    driver = GPU_DRIVER_OFFICIAL;
  }
  else if (strstr(renderer, "Apple Software Renderer")) {
    device = GPU_DEVICE_SOFTWARE;
    driver = GPU_DRIVER_SOFTWARE;
  }
  else if (strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe")) {
    device = GPU_DEVICE_SOFTWARE;
    driver = GPU_DRIVER_SOFTWARE;
  }
  else {
    printf("Warning: Could not find a matching GPU name. Things may not behave as expected.\n");
    printf("Detected OpenGL configuration:\n");
    printf("Vendor: %s\n", vendor);
    printf("Renderer: %s\n", renderer);
  }

  /* Detect support level */
  if (!(epoxy_gl_version() >= 43)) {
    support_level = GPU_SUPPORT_LEVEL_UNSUPPORTED;
  }
  else {
#if defined(WIN32)
    long long driverVersion = 0;
    if (device & GPU_DEVICE_QUALCOMM) {
      if (BLI_windows_get_directx_driver_version(L"Qualcomm(R) Adreno(TM)", &driverVersion)) {
        /* Parse out the driver version in format x.x.x.x */
        WORD ver0 = (driverVersion >> 48) & 0xffff;
        WORD ver1 = (driverVersion >> 32) & 0xffff;
        WORD ver2 = (driverVersion >> 16) & 0xffff;

        /* Any Qualcomm driver older than 30.x.x.x will never capable of running blender >= 4.0
         * As due to an issue in D3D typed UAV load capabilities, Compute Shaders are not available
         * 30.0.3820.x and above are capable of running blender >=4.0, but these drivers
         * are only available on 8cx gen3 devices or newer */
        if (ver0 < 30 || (ver0 == 30 && ver1 == 0 && ver2 < 3820)) {
          std::cout
              << "=====================================\n"
              << "Qualcomm drivers older than 30.0.3820.x are not capable of running Blender 4.0\n"
              << "If your device is older than an 8cx Gen3, you must use a 3.x LTS release.\n"
              << "If you have an 8cx Gen3 or newer device, a driver update may be available.\n"
              << "=====================================\n";
          support_level = GPU_SUPPORT_LEVEL_UNSUPPORTED;
        }
      }
    }
#endif
    if ((device & GPU_DEVICE_INTEL) && (os & GPU_OS_WIN)) {
      /* Old Intel drivers with known bugs that cause material properties to crash.
       * Version Build 10.18.14.5067 is the latest available and appears to be working
       * ok with our workarounds, so excluded from this list. */
      if (strstr(version, "Build 7.14") || strstr(version, "Build 7.15") ||
          strstr(version, "Build 8.15") || strstr(version, "Build 9.17") ||
          strstr(version, "Build 9.18") || strstr(version, "Build 10.18.10.3") ||
          strstr(version, "Build 10.18.10.4") || strstr(version, "Build 10.18.10.5") ||
          strstr(version, "Build 10.18.14.4"))
      {
        support_level = GPU_SUPPORT_LEVEL_LIMITED;
      }
      /* A rare GPU that has z-fighting issues in edit mode. (see #128179) */
      if (strstr(renderer, "HD Graphics 405")) {
        support_level = GPU_SUPPORT_LEVEL_LIMITED;
      }
      /* Latest Intel driver have bugs that won't allow Blender to start.
       * Users must install different version of the driver.
       * See #113124 for more information. */
      if (strstr(version, "Build 20.19.15.51")) {
        support_level = GPU_SUPPORT_LEVEL_UNSUPPORTED;
      }
    }
    if ((device & GPU_DEVICE_ATI) && (os & GPU_OS_UNIX)) {
      /* Platform seems to work when SB backend is disabled. This can be done
       * by adding the environment variable `R600_DEBUG=nosb`. */
      if (strstr(renderer, "AMD CEDAR")) {
        support_level = GPU_SUPPORT_LEVEL_LIMITED;
      }
    }

    /* Check SSBO bindings requirement. */
    GLint max_ssbo_binds_vertex;
    GLint max_ssbo_binds_fragment;
    GLint max_ssbo_binds_compute;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &max_ssbo_binds_vertex);
    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &max_ssbo_binds_fragment);
    glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &max_ssbo_binds_compute);
    GLint max_ssbo_binds = min_iii(
        max_ssbo_binds_vertex, max_ssbo_binds_fragment, max_ssbo_binds_compute);
    if (max_ssbo_binds < 12) {
      std::cout << "Warning: Unsupported platform as it supports max " << max_ssbo_binds
                << " SSBO binding locations\n";
      support_level = GPU_SUPPORT_LEVEL_UNSUPPORTED;
    }
  }

  /* Compute shaders have some issues with those versions (see #94936). */
  if ((device & GPU_DEVICE_ATI) && (driver & GPU_DRIVER_OFFICIAL) &&
      (strstr(version, "4.5.14831") || strstr(version, "4.5.14760")))
  {
    support_level = GPU_SUPPORT_LEVEL_UNSUPPORTED;
  }

  GPG.init(device,
           os,
           driver,
           support_level,
           GPU_BACKEND_OPENGL,
           vendor,
           renderer,
           version,
           GPU_ARCHITECTURE_IMR);
}

void GLBackend::platform_exit()
{
  BLI_assert(GPG.initialized);
  GPG.clear();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Capabilities
 * \{ */

static bool detect_mip_render_workaround()
{
  int cube_size = 2;
  float clear_color[4] = {1.0f, 0.5f, 0.0f, 0.0f};
  float *source_pix = (float *)MEM_callocN(sizeof(float[4]) * cube_size * cube_size * 6, __func__);

  /* NOTE: Debug layers are not yet enabled. Force use of glGetError. */
  debug::check_gl_error("Cubemap Workaround Start");
  /* Not using GPU API since it is not yet fully initialized. */
  GLuint tex, fb;
  /* Create cubemap with 2 mip level. */
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
  for (int mip = 0; mip < 2; mip++) {
    for (int i = 0; i < 6; i++) {
      const int width = cube_size / (1 << mip);
      GLenum target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + i;
      glTexImage2D(target, mip, GL_RGBA16F, width, width, 0, GL_RGBA, GL_FLOAT, source_pix);
    }
  }
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 0);
  /* Attach and clear mip 1. */
  glGenFramebuffers(1, &fb);
  glBindFramebuffer(GL_FRAMEBUFFER, fb);
  glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 1);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);
  glClearColor(UNPACK4(clear_color));
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  /* Read mip 1. If color is not the same as the clear_color, the rendering failed. */
  glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 1, GL_RGBA, GL_FLOAT, source_pix);
  bool enable_workaround = !equals_v4v4(clear_color, source_pix);
  MEM_freeN(source_pix);

  glDeleteFramebuffers(1, &fb);
  glDeleteTextures(1, &tex);

  debug::check_gl_error("Cubemap Workaround End9");

  return enable_workaround;
}

static const char *gl_extension_get(int i)
{
  return (char *)glGetStringi(GL_EXTENSIONS, i);
}

static void detect_workarounds()
{
  const char *vendor = (const char *)glGetString(GL_VENDOR);
  const char *renderer = (const char *)glGetString(GL_RENDERER);
  const char *version = (const char *)glGetString(GL_VERSION);

  if (G.debug & G_DEBUG_GPU_FORCE_WORKAROUNDS) {
    printf("\n");
    printf("GL: Forcing workaround usage and disabling extensions.\n");
    printf("    OpenGL identification strings\n");
    printf("    vendor: %s\n", vendor);
    printf("    renderer: %s\n", renderer);
    printf("    version: %s\n\n", version);
    GCaps.depth_blitting_workaround = true;
    GCaps.mip_render_workaround = true;
    GCaps.stencil_clasify_buffer_workaround = true;
    GCaps.node_link_instancing_workaround = true;
    GCaps.line_directive_workaround = true;
    GLContext::debug_layer_workaround = true;
    /* Turn off Blender features. */
    GCaps.hdr_viewport_support = false;
    /* Turn off OpenGL 4.4 features. */
    GLContext::multi_bind_support = false;
    GLContext::multi_bind_image_support = false;
    /* Turn off OpenGL 4.5 features. */
    GLContext::direct_state_access_support = false;
    /* Turn off OpenGL 4.6 features. */
    GLContext::texture_filter_anisotropic_support = false;
    GCaps.shader_draw_parameters_support = false;
    GLContext::shader_draw_parameters_support = false;
    /* Although an OpenGL 4.3 feature, our implementation requires shader_draw_parameters_support.
     * NOTE: we should untangle this by checking both features for clarity. */
    GLContext::multi_draw_indirect_support = false;
    /* Turn off extensions. */
    GLContext::layered_rendering_support = false;
    /* Turn off vendor specific extensions. */
    GLContext::native_barycentric_support = false;
    GLContext::framebuffer_fetch_support = false;
    GLContext::texture_barrier_support = false;
    GCaps.stencil_export_support = false;

#if 0
    /* Do not alter OpenGL 4.3 features.
     * These code paths should be removed. */
    GLContext::debug_layer_support = false;
#endif

    return;
  }

  if (GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_WIN, GPU_DRIVER_OFFICIAL) &&
      (strstr(version, "4.5.13399") || strstr(version, "4.5.13417") ||
       strstr(version, "4.5.13422") || strstr(version, "4.5.13467")))
  {
    /* The renderers include:
     *   Radeon HD 5000;
     *   Radeon HD 7500M;
     *   Radeon HD 7570M;
     *   Radeon HD 7600M;
     *   Radeon R5 Graphics;
     * And others... */
    GLContext::unused_fb_slot_workaround = true;
    GCaps.mip_render_workaround = true;
    GCaps.shader_draw_parameters_support = false;
    GCaps.broken_amd_driver = true;
  }
  /* We have issues with this specific renderer. (see #74024) */
  if (GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_UNIX, GPU_DRIVER_OPENSOURCE) &&
      (strstr(renderer, "AMD VERDE") || strstr(renderer, "AMD KAVERI") ||
       strstr(renderer, "AMD TAHITI")))
  {
    GLContext::unused_fb_slot_workaround = true;
    GCaps.shader_draw_parameters_support = false;
    GCaps.broken_amd_driver = true;
  }
  /* Fix slowdown on this particular driver. (see #77641) */
  if (GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_UNIX, GPU_DRIVER_OPENSOURCE) &&
      strstr(version, "Mesa 19.3.4"))
  {
    GCaps.shader_draw_parameters_support = false;
    GCaps.broken_amd_driver = true;
  }
  /* See #82856: AMD drivers since 20.11 running on a polaris architecture doesn't support the
   * `GL_INT_2_10_10_10_REV` data type correctly. This data type is used to pack normals and flags.
   * The work around uses `GPU_RGBA16I`. In 22.?.? drivers this has been fixed for
   * polaris platform. Keeping legacy platforms around just in case.
   */
  if (GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_ANY, GPU_DRIVER_OFFICIAL)) {
    /* Check for AMD legacy driver. Assuming that when these drivers are used this bug is present.
     */
    if (strstr(version, " 22.6.1 ") || strstr(version, " 21.Q1.2 ") ||
        strstr(version, " 21.Q2.1 "))
    {
      GCaps.use_hq_normals_workaround = true;
    }
    const Vector<std::string> matches = {
        "RX550/550", "(TM) 520", "(TM) 530", "(TM) 535", "R5", "R7", "R9", "HD"};

    if (match_renderer(renderer, matches)) {
      GCaps.use_hq_normals_workaround = true;
    }
  }
  /* See #132968: Legacy AMD drivers do not accept a hash after the line number and results into
   * undefined behaviour. Users have reported that the issue can go away after doing a clean
   * install of the driver.
   */
  if (GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_ANY, GPU_DRIVER_OFFICIAL)) {
    if (strstr(version, " 22.6.1 ") || strstr(version, " 21.Q1.2 ") ||
        strstr(version, " 21.Q2.1 "))
    {
      GCaps.line_directive_workaround = true;
    }
  }

  /* Special fix for these specific GPUs.
   * Without this workaround, blender crashes on startup. (see #72098) */
  if (GPU_type_matches(GPU_DEVICE_INTEL, GPU_OS_WIN, GPU_DRIVER_OFFICIAL) &&
      (strstr(renderer, "HD Graphics 620") || strstr(renderer, "HD Graphics 630")))
  {
    GCaps.mip_render_workaround = true;
  }
  /* Maybe not all of these drivers have problems with `GL_ARB_base_instance`.
   * But it's hard to test each case.
   * We get crashes from some crappy Intel drivers don't work well with shaders created in
   * different rendering contexts. */
  if (GPU_type_matches(GPU_DEVICE_INTEL, GPU_OS_WIN, GPU_DRIVER_ANY) &&
      (strstr(version, "Build 10.18.10.3") || strstr(version, "Build 10.18.10.4") ||
       strstr(version, "Build 10.18.10.5") || strstr(version, "Build 10.18.14.4") ||
       strstr(version, "Build 10.18.14.5")))
  {
    GCaps.use_main_context_workaround = true;
  }
  /* Somehow fixes armature display issues (see #69743). */
  if (GPU_type_matches(GPU_DEVICE_INTEL, GPU_OS_WIN, GPU_DRIVER_ANY) &&
      strstr(version, "Build 20.19.15.4285"))
  {
    GCaps.use_main_context_workaround = true;
  }
  /* See #70187: merging vertices fail. This has been tested from `18.2.2` till `19.3.0~dev`
   * of the Mesa driver */
  if (GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_UNIX, GPU_DRIVER_OPENSOURCE) &&
      (strstr(version, "Mesa 18.") || strstr(version, "Mesa 19.0") ||
       strstr(version, "Mesa 19.1") || strstr(version, "Mesa 19.2")))
  {
    GLContext::unused_fb_slot_workaround = true;
  }

  /* Draw shader parameters are broken on Qualcomm Windows ARM64 devices
   * on Mesa version < 24.0.0 */
  if (GPU_type_matches(GPU_DEVICE_QUALCOMM, GPU_OS_WIN, GPU_DRIVER_ANY)) {
    if (strstr(version, "Mesa 20.") || strstr(version, "Mesa 21.") ||
        strstr(version, "Mesa 22.") || strstr(version, "Mesa 23."))
    {
      GCaps.shader_draw_parameters_support = false;
      GLContext::shader_draw_parameters_support = false;
    }
  }

/* Snapdragon X Elite devices currently have a driver bug that results in
 * eevee rendering a black cube with anything except an emission shader
 * if shader draw parameters are enabled (#122837) */
#if defined(WIN32)
  long long driverVersion = 0;
  if (GPU_type_matches(GPU_DEVICE_QUALCOMM, GPU_OS_WIN, GPU_DRIVER_ANY)) {
    if (BLI_windows_get_directx_driver_version(L"Qualcomm(R) Adreno(TM)", &driverVersion)) {
      /* Parse out the driver version */
      WORD ver0 = (driverVersion >> 48) & 0xffff;

      /* X Elite devices have GPU driver version 31, and currently no known release version of the
       * GPU driver renders the cube correctly. This will be changed when a working driver version
       * is released to commercial devices to only enable this flags on older drivers. */
      if (ver0 == 31) {
        GCaps.stencil_clasify_buffer_workaround = true;
      }
    }
  }
#endif

  /* Some Intel drivers have issues with using mips as frame-buffer targets if
   * GL_TEXTURE_MAX_LEVEL is higher than the target MIP.
   * Only check at the end after all other workarounds because this uses the drawing code.
   * Also after device/driver flags to avoid the check that causes pre GCN Radeon to crash. */
  if (GCaps.mip_render_workaround == false) {
    GCaps.mip_render_workaround = detect_mip_render_workaround();
  }
  /* Disable multi-draw if the base instance cannot be read. */
  if (GLContext::shader_draw_parameters_support == false) {
    GLContext::multi_draw_indirect_support = false;
  }
  /* Enable our own incomplete debug layer if no other is available. */
  if (GLContext::debug_layer_support == false) {
    GLContext::debug_layer_workaround = true;
  }

  /* There is an issue in AMD official driver where we cannot use multi bind when using images. AMD
   * is aware of the issue, but hasn't released a fix. */
  if (GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_ANY, GPU_DRIVER_OFFICIAL)) {
    GLContext::multi_bind_image_support = false;
  }

  /* #107642, #120273 Windows Intel iGPU (multiple generations) incorrectly report that
   * they support image binding. But when used it results into `GL_INVALID_OPERATION` with
   * `internal format of texture N is not supported`. */
  if (GPU_type_matches(GPU_DEVICE_INTEL, GPU_OS_WIN, GPU_DRIVER_OFFICIAL)) {
    GLContext::multi_bind_image_support = false;
  }

  /* #134509 Intel ARC GPU have a driver bug that break the display of batched node-links.
   * Disabling batching fixes the issue. */
  if (GPU_type_matches(GPU_DEVICE_INTEL, GPU_OS_ANY, GPU_DRIVER_OFFICIAL)) {
    GCaps.node_link_instancing_workaround = true;
  }

  /* Metal-related Workarounds. */

  /* Minimum Per-Vertex stride is 1 byte for OpenGL. */
  GCaps.minimum_per_vertex_stride = 1;
}

/** Internal capabilities. */

GLint GLContext::max_cubemap_size = 0;
GLint GLContext::max_ubo_binds = 0;
GLint GLContext::max_ubo_size = 0;
GLint GLContext::max_ssbo_binds = 0;

/** Extensions. */

bool GLContext::debug_layer_support = false;
bool GLContext::direct_state_access_support = false;
bool GLContext::explicit_location_support = false;
bool GLContext::framebuffer_fetch_support = false;
bool GLContext::layered_rendering_support = false;
bool GLContext::native_barycentric_support = false;
bool GLContext::multi_bind_support = false;
bool GLContext::multi_bind_image_support = false;
bool GLContext::multi_draw_indirect_support = false;
bool GLContext::shader_draw_parameters_support = false;
bool GLContext::stencil_texturing_support = false;
bool GLContext::texture_barrier_support = false;
bool GLContext::texture_filter_anisotropic_support = false;

/** Workarounds. */

bool GLContext::debug_layer_workaround = false;
bool GLContext::unused_fb_slot_workaround = false;
bool GLContext::generate_mipmap_workaround = false;

void GLBackend::capabilities_init()
{
  BLI_assert(epoxy_gl_version() >= 33);
  /* Common Capabilities. */
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &GCaps.max_texture_size);
  glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &GCaps.max_texture_layers);
  glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &GCaps.max_textures_frag);
  glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &GCaps.max_textures_vert);
  glGetIntegerv(GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS, &GCaps.max_textures_geom);
  glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &GCaps.max_textures);
  glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, &GCaps.max_uniforms_vert);
  glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, &GCaps.max_uniforms_frag);
  glGetIntegerv(GL_MAX_ELEMENTS_INDICES, &GCaps.max_batch_indices);
  glGetIntegerv(GL_MAX_ELEMENTS_VERTICES, &GCaps.max_batch_vertices);
  glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &GCaps.max_vertex_attribs);
  glGetIntegerv(GL_MAX_VARYING_FLOATS, &GCaps.max_varying_floats);
  glGetIntegerv(GL_MAX_IMAGE_UNITS, &GCaps.max_images);

  glGetIntegerv(GL_NUM_EXTENSIONS, &GCaps.extensions_len);
  GCaps.extension_get = gl_extension_get;

  GCaps.max_samplers = GCaps.max_textures;
  GCaps.mem_stats_support = epoxy_has_gl_extension("GL_NVX_gpu_memory_info") ||
                            epoxy_has_gl_extension("GL_ATI_meminfo");
  GCaps.shader_draw_parameters_support = epoxy_has_gl_extension("GL_ARB_shader_draw_parameters");
  GCaps.geometry_shader_support = true;
  GCaps.max_samplers = GCaps.max_textures;
  GCaps.hdr_viewport_support = false;

  glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &GCaps.max_work_group_count[0]);
  glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 1, &GCaps.max_work_group_count[1]);
  glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 2, &GCaps.max_work_group_count[2]);
  glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0, &GCaps.max_work_group_size[0]);
  glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1, &GCaps.max_work_group_size[1]);
  glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 2, &GCaps.max_work_group_size[2]);
  glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &GCaps.max_shader_storage_buffer_bindings);
  glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &GCaps.max_compute_shader_storage_blocks);
  int64_t max_ssbo_size;
  glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_ssbo_size);
  GCaps.max_storage_buffer_size = size_t(max_ssbo_size);
  GLint ssbo_alignment;
  glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssbo_alignment);
  GCaps.storage_buffer_alignment = size_t(ssbo_alignment);

  GCaps.texture_view_support = epoxy_gl_version() >= 43 ||
                               epoxy_has_gl_extension("GL_ARB_texture_view");
  GCaps.stencil_export_support = epoxy_has_gl_extension("GL_ARB_shader_stencil_export");

  /* GL specific capabilities. */
  glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &GCaps.max_texture_3d_size);
  glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &GLContext::max_cubemap_size);
  glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, &GLContext::max_ubo_binds);
  glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &GLContext::max_ubo_size);
  GLint max_ssbo_binds;
  GLContext::max_ssbo_binds = 999999;
  glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &max_ssbo_binds);
  GLContext::max_ssbo_binds = min_ii(GLContext::max_ssbo_binds, max_ssbo_binds);
  glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &max_ssbo_binds);
  GLContext::max_ssbo_binds = min_ii(GLContext::max_ssbo_binds, max_ssbo_binds);
  glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &max_ssbo_binds);
  GLContext::max_ssbo_binds = min_ii(GLContext::max_ssbo_binds, max_ssbo_binds);
  GLContext::debug_layer_support = epoxy_gl_version() >= 43 ||
                                   epoxy_has_gl_extension("GL_KHR_debug") ||
                                   epoxy_has_gl_extension("GL_ARB_debug_output");
  GLContext::direct_state_access_support = epoxy_has_gl_extension("GL_ARB_direct_state_access");
  GLContext::explicit_location_support = epoxy_gl_version() >= 43;
  GLContext::framebuffer_fetch_support = epoxy_has_gl_extension("GL_EXT_shader_framebuffer_fetch");
  GLContext::texture_barrier_support = epoxy_has_gl_extension("GL_ARB_texture_barrier");
  GLContext::layered_rendering_support = epoxy_has_gl_extension(
      "GL_ARB_shader_viewport_layer_array");
  GLContext::native_barycentric_support = epoxy_has_gl_extension(
      "GL_AMD_shader_explicit_vertex_parameter");
  GLContext::multi_bind_support = GLContext::multi_bind_image_support = epoxy_has_gl_extension(
      "GL_ARB_multi_bind");
  GLContext::multi_draw_indirect_support = epoxy_has_gl_extension("GL_ARB_multi_draw_indirect");
  GLContext::shader_draw_parameters_support = epoxy_has_gl_extension(
      "GL_ARB_shader_draw_parameters");
  GLContext::stencil_texturing_support = epoxy_gl_version() >= 43;
  GLContext::texture_filter_anisotropic_support = epoxy_has_gl_extension(
      "GL_EXT_texture_filter_anisotropic");

  /* Disabled until it is proven to work. */
  GLContext::framebuffer_fetch_support = false;

  detect_workarounds();

#if BLI_SUBPROCESS_SUPPORT
  if (GCaps.max_parallel_compilations == -1) {
    GCaps.max_parallel_compilations = std::min(int(U.max_shader_compilation_subprocesses),
                                               BLI_system_thread_count());
  }
  if (G.debug & G_DEBUG_GPU_RENDERDOC) {
    /* Avoid crashes on RenderDoc sessions. */
    GCaps.max_parallel_compilations = 0;
  }
#else
  GCaps.max_parallel_compilations = 0;
#endif

  /* Disable this feature entirely when not debugging. */
  if ((G.debug & G_DEBUG_GPU) == 0) {
    GLContext::debug_layer_support = false;
    GLContext::debug_layer_workaround = false;
  }
}

/** \} */

}  // namespace blender::gpu
