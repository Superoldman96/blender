/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_ContextVK.hh"

#ifdef _WIN32
#  include <vulkan/vulkan_win32.h>
#elif defined(__APPLE__)
#  include <MoltenVK/vk_mvk_moltenvk.h>
#else /* X11/WAYLAND. */
#  ifdef WITH_GHOST_X11
#    include <vulkan/vulkan_xlib.h>
#  endif
#  ifdef WITH_GHOST_WAYLAND
#    include <vulkan/vulkan_wayland.h>
#  endif
#endif

#include "vulkan/vk_ghost_api.hh"

#include <vector>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>

#include <sys/stat.h>

using namespace std;

static const char *vulkan_error_as_string(VkResult result)
{
#define FORMAT_ERROR(X) \
  case X: { \
    return "" #X; \
  }

  switch (result) {
    FORMAT_ERROR(VK_NOT_READY);
    FORMAT_ERROR(VK_TIMEOUT);
    FORMAT_ERROR(VK_EVENT_SET);
    FORMAT_ERROR(VK_EVENT_RESET);
    FORMAT_ERROR(VK_INCOMPLETE);
    FORMAT_ERROR(VK_ERROR_OUT_OF_HOST_MEMORY);
    FORMAT_ERROR(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    FORMAT_ERROR(VK_ERROR_INITIALIZATION_FAILED);
    FORMAT_ERROR(VK_ERROR_DEVICE_LOST);
    FORMAT_ERROR(VK_ERROR_MEMORY_MAP_FAILED);
    FORMAT_ERROR(VK_ERROR_LAYER_NOT_PRESENT);
    FORMAT_ERROR(VK_ERROR_EXTENSION_NOT_PRESENT);
    FORMAT_ERROR(VK_ERROR_FEATURE_NOT_PRESENT);
    FORMAT_ERROR(VK_ERROR_INCOMPATIBLE_DRIVER);
    FORMAT_ERROR(VK_ERROR_TOO_MANY_OBJECTS);
    FORMAT_ERROR(VK_ERROR_FORMAT_NOT_SUPPORTED);
    FORMAT_ERROR(VK_ERROR_FRAGMENTED_POOL);
    FORMAT_ERROR(VK_ERROR_UNKNOWN);
    FORMAT_ERROR(VK_ERROR_OUT_OF_POOL_MEMORY);
    FORMAT_ERROR(VK_ERROR_INVALID_EXTERNAL_HANDLE);
    FORMAT_ERROR(VK_ERROR_FRAGMENTATION);
    FORMAT_ERROR(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);
    FORMAT_ERROR(VK_ERROR_SURFACE_LOST_KHR);
    FORMAT_ERROR(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
    FORMAT_ERROR(VK_SUBOPTIMAL_KHR);
    FORMAT_ERROR(VK_ERROR_OUT_OF_DATE_KHR);
    FORMAT_ERROR(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR);
    FORMAT_ERROR(VK_ERROR_VALIDATION_FAILED_EXT);
    FORMAT_ERROR(VK_ERROR_INVALID_SHADER_NV);
    FORMAT_ERROR(VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
    FORMAT_ERROR(VK_ERROR_NOT_PERMITTED_EXT);
    FORMAT_ERROR(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT);
    FORMAT_ERROR(VK_THREAD_IDLE_KHR);
    FORMAT_ERROR(VK_THREAD_DONE_KHR);
    FORMAT_ERROR(VK_OPERATION_DEFERRED_KHR);
    FORMAT_ERROR(VK_OPERATION_NOT_DEFERRED_KHR);
    FORMAT_ERROR(VK_PIPELINE_COMPILE_REQUIRED_EXT);
    default:
      return "Unknown Error";
  }
}

#define __STR(A) "" #A
#define VK_CHECK(__expression) \
  do { \
    VkResult r = (__expression); \
    if (r != VK_SUCCESS) { \
      fprintf(stderr, \
              "Vulkan Error : %s:%d : %s failed with %s\n", \
              __FILE__, \
              __LINE__, \
              __STR(__expression), \
              vulkan_error_as_string(r)); \
      return GHOST_kFailure; \
    } \
  } while (0)

#define DEBUG_PRINTF(...) \
  if (m_debug) { \
    printf(__VA_ARGS__); \
  }

/* Check if the given extension name is in the extension_list.
 */
static bool contains_extension(const vector<VkExtensionProperties> &extension_list,
                               const char *extension_name)
{
  for (const VkExtensionProperties &extension_properties : extension_list) {
    if (strcmp(extension_properties.extensionName, extension_name) == 0) {
      return true;
    }
  }
  return false;
};

/* -------------------------------------------------------------------- */
/** \name Vulkan Device
 * \{ */

class GHOST_DeviceVK {
 public:
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;

  VkDevice device = VK_NULL_HANDLE;

  uint32_t generic_queue_family = 0;

  VkPhysicalDeviceProperties properties = {};
  VkPhysicalDeviceFeatures2 features = {};
  VkPhysicalDeviceVulkan11Features features_11 = {};
  VkPhysicalDeviceVulkan12Features features_12 = {};

  int users = 0;

  /** Mutex to externally synchronize access to queue. */
  std::mutex queue_mutex;

  bool use_vk_ext_swapchain_maintenance_1 = false;

 public:
  GHOST_DeviceVK(VkInstance vk_instance, VkPhysicalDevice vk_physical_device)
      : instance(vk_instance), physical_device(vk_physical_device)
  {
    vkGetPhysicalDeviceProperties(physical_device, &properties);

    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features_11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features.pNext = &features_11;
    features_11.pNext = &features_12;

    vkGetPhysicalDeviceFeatures2(physical_device, &features);
  }
  ~GHOST_DeviceVK()
  {
    if (device != VK_NULL_HANDLE) {
      vkDestroyDevice(device, nullptr);
    }
  }

  void wait_idle()
  {
    if (device) {
      vkDeviceWaitIdle(device);
    }
  }

  bool has_extensions(const vector<const char *> &required_extensions)
  {
    uint32_t ext_count;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, nullptr);

    vector<VkExtensionProperties> available_exts(ext_count);
    vkEnumerateDeviceExtensionProperties(
        physical_device, nullptr, &ext_count, available_exts.data());

    for (const auto &extension_needed : required_extensions) {
      bool found = false;
      for (const auto &extension : available_exts) {
        if (strcmp(extension_needed, extension.extensionName) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        return false;
      }
    }
    return true;
  }

  void ensure_device(vector<const char *> &required_extensions,
                     vector<const char *> &optional_extensions)
  {
    if (device != VK_NULL_HANDLE) {
      return;
    }
    init_generic_queue_family();

    vector<VkDeviceQueueCreateInfo> queue_create_infos;
    vector<const char *> device_extensions(required_extensions);
    for (const char *optional_extension : optional_extensions) {
      if (has_extensions({optional_extension})) {
        device_extensions.push_back(optional_extension);
      }
    }

    /* Check if the given extension name will be enabled. */
    auto extension_requested = [=](const char *extension_name) {
      for (const char *device_extension_name : device_extensions) {
        if (strcmp(device_extension_name, extension_name) == 0) {
          return true;
        }
      }
      return false;
    };

    float queue_priorities[] = {1.0f};
    VkDeviceQueueCreateInfo graphic_queue_create_info = {};
    graphic_queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    graphic_queue_create_info.queueFamilyIndex = generic_queue_family;
    graphic_queue_create_info.queueCount = 1;
    graphic_queue_create_info.pQueuePriorities = queue_priorities;
    queue_create_infos.push_back(graphic_queue_create_info);

    VkPhysicalDeviceFeatures device_features = {};
#ifndef __APPLE__
    device_features.geometryShader = VK_TRUE;
    /* MoltenVK supports logicOp, needs to be build with MVK_USE_METAL_PRIVATE_API. */
    device_features.logicOp = VK_TRUE;
#endif
    device_features.dualSrcBlend = VK_TRUE;
    device_features.imageCubeArray = VK_TRUE;
    device_features.multiDrawIndirect = VK_TRUE;
    device_features.multiViewport = VK_TRUE;
    device_features.shaderClipDistance = VK_TRUE;
    device_features.drawIndirectFirstInstance = VK_TRUE;
    device_features.fragmentStoresAndAtomics = VK_TRUE;
    device_features.samplerAnisotropy = features.features.samplerAnisotropy;

    VkDeviceCreateInfo device_create_info = {};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = uint32_t(queue_create_infos.size());
    device_create_info.pQueueCreateInfos = queue_create_infos.data();
    device_create_info.enabledExtensionCount = uint32_t(device_extensions.size());
    device_create_info.ppEnabledExtensionNames = device_extensions.data();
    device_create_info.pEnabledFeatures = &device_features;

    void *device_create_info_p_next = nullptr;

    /* Enable vulkan 11 features when supported on physical device. */
    VkPhysicalDeviceVulkan11Features vulkan_11_features = {};
    vulkan_11_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vulkan_11_features.pNext = device_create_info_p_next;
    vulkan_11_features.shaderDrawParameters = features_11.shaderDrawParameters;
    device_create_info_p_next = &vulkan_11_features;

    /* Enable optional vulkan 12 features when supported on physical device. */
    VkPhysicalDeviceVulkan12Features vulkan_12_features = {};
    vulkan_12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan_12_features.shaderOutputLayer = features_12.shaderOutputLayer;
    vulkan_12_features.shaderOutputViewportIndex = features_12.shaderOutputViewportIndex;
    vulkan_12_features.timelineSemaphore = VK_TRUE;
    vulkan_12_features.pNext = device_create_info_p_next;
    device_create_info_p_next = &vulkan_12_features;

    /* Enable provoking vertex. */
    VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_features = {};
    provoking_vertex_features.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT;
    provoking_vertex_features.provokingVertexLast = VK_TRUE;
    provoking_vertex_features.pNext = device_create_info_p_next;
    device_create_info_p_next = &provoking_vertex_features;

    /* Enable dynamic rendering. */
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering = {};
    dynamic_rendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamic_rendering.dynamicRendering = VK_TRUE;
    if (has_extensions({VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME})) {
      dynamic_rendering.pNext = device_create_info_p_next;
      device_create_info_p_next = &dynamic_rendering;
    }

    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT
        dynamic_rendering_unused_attachments = {};
    dynamic_rendering_unused_attachments.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT;
    dynamic_rendering_unused_attachments.dynamicRenderingUnusedAttachments = VK_TRUE;
    if (has_extensions({VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME})) {
      dynamic_rendering_unused_attachments.pNext = device_create_info_p_next;
      device_create_info_p_next = &dynamic_rendering_unused_attachments;
    }

    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR dynamic_rendering_local_read = {};
    dynamic_rendering_local_read.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR;
    dynamic_rendering_local_read.dynamicRenderingLocalRead = VK_TRUE;
    if (has_extensions({VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME})) {
      dynamic_rendering_local_read.pNext = device_create_info_p_next;
      device_create_info_p_next = &dynamic_rendering_local_read;
    }

    /* Query for Mainenance4 (core in Vulkan 1.3). */
    VkPhysicalDeviceMaintenance4FeaturesKHR maintenance_4 = {};
    maintenance_4.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR;
    maintenance_4.maintenance4 = VK_TRUE;
    if (has_extensions({VK_KHR_MAINTENANCE_4_EXTENSION_NAME})) {
      maintenance_4.pNext = device_create_info_p_next;
      device_create_info_p_next = &maintenance_4;
    }

    /* Swap-chain maintenance 1 is optional. */
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchain_maintenance_1 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT, nullptr, VK_TRUE};
    if (extension_requested(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
      swapchain_maintenance_1.pNext = device_create_info_p_next;
      device_create_info_p_next = &swapchain_maintenance_1;
      use_vk_ext_swapchain_maintenance_1 = true;
    }

    /* Query and enable Fragment Shader Barycentrics. */
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR fragment_shader_barycentric = {};
    fragment_shader_barycentric.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR;
    fragment_shader_barycentric.fragmentShaderBarycentric = VK_TRUE;
    if (has_extensions({VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME})) {
      fragment_shader_barycentric.pNext = device_create_info_p_next;
      device_create_info_p_next = &fragment_shader_barycentric;
    }

    device_create_info.pNext = device_create_info_p_next;
    vkCreateDevice(physical_device, &device_create_info, nullptr, &device);
  }

  void init_generic_queue_family()
  {
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);

    vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &queue_family_count, queue_families.data());

    generic_queue_family = 0;
    for (const auto &queue_family : queue_families) {
      /* Every VULKAN implementation by spec must have one queue family that support both graphics
       * and compute pipelines. We select this one; compute only queue family hints at asynchronous
       * compute implementations. */
      if ((queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
          (queue_family.queueFlags & VK_QUEUE_COMPUTE_BIT))
      {
        return;
      }
      generic_queue_family++;
    }

    fprintf(stderr, "Couldn't find any Graphic queue family on selected device\n");
  }
};

/**
 * A shared device between multiple contexts.
 *
 * The logical device needs to be shared as multiple contexts can be created and the logical vulkan
 * device they share should be the same otherwise memory operations might be done on the incorrect
 * device.
 */
static std::optional<GHOST_DeviceVK> vulkan_device;

static GHOST_TSuccess ensure_vulkan_device(VkInstance vk_instance,
                                           VkSurfaceKHR vk_surface,
                                           const GHOST_GPUDevice &preferred_device,
                                           const vector<const char *> &required_extensions)
{
  if (vulkan_device.has_value()) {
    return GHOST_kSuccess;
  }

  VkPhysicalDevice best_physical_device = VK_NULL_HANDLE;

  uint32_t device_count = 0;
  vkEnumeratePhysicalDevices(vk_instance, &device_count, nullptr);

  vector<VkPhysicalDevice> physical_devices(device_count);
  vkEnumeratePhysicalDevices(vk_instance, &device_count, physical_devices.data());

  int best_device_score = -1;
  int device_index = -1;
  for (const auto &physical_device : physical_devices) {
    GHOST_DeviceVK device_vk(vk_instance, physical_device);
    device_index++;

    if (!device_vk.has_extensions(required_extensions)) {
      continue;
    }
    if (!blender::gpu::GPU_vulkan_is_supported_driver(physical_device)) {
      continue;
    }

    if (vk_surface != VK_NULL_HANDLE) {
      uint32_t format_count;
      vkGetPhysicalDeviceSurfaceFormatsKHR(
          device_vk.physical_device, vk_surface, &format_count, nullptr);

      uint32_t present_count;
      vkGetPhysicalDeviceSurfacePresentModesKHR(
          device_vk.physical_device, vk_surface, &present_count, nullptr);

      /* For now anything will do. */
      if (format_count == 0 || present_count == 0) {
        continue;
      }
    }

#ifdef __APPLE__
    if (!device_vk.features.features.dualSrcBlend || !device_vk.features.features.imageCubeArray) {
      continue;
    }
#else
    if (!device_vk.features.features.geometryShader || !device_vk.features.features.dualSrcBlend ||
        !device_vk.features.features.logicOp || !device_vk.features.features.imageCubeArray)
    {
      continue;
    }
#endif

    int device_score = 0;
    switch (device_vk.properties.deviceType) {
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        device_score = 400;
        break;
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        device_score = 300;
        break;
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        device_score = 200;
        break;
      case VK_PHYSICAL_DEVICE_TYPE_CPU:
        device_score = 100;
        break;
      default:
        break;
    }
    /* User has configured a preferred device. Add bonus score when vendor and device match. Driver
     * id isn't considered as drivers update more frequently and can break the device selection. */
    if (device_vk.properties.deviceID == preferred_device.device_id &&
        device_vk.properties.vendorID == preferred_device.vendor_id)
    {
      device_score += 500;
      if (preferred_device.index == device_index) {
        device_score += 10;
      }
    }
    if (device_score > best_device_score) {
      best_physical_device = physical_device;
      best_device_score = device_score;
    }
  }

  if (best_physical_device == VK_NULL_HANDLE) {
    fprintf(stderr, "Error: No suitable Vulkan Device found!\n");
    return GHOST_kFailure;
  }

  vulkan_device.emplace(vk_instance, best_physical_device);

  return GHOST_kSuccess;
}

/** \} */

GHOST_ContextVK::GHOST_ContextVK(bool stereoVisual,
#ifdef _WIN32
                                 HWND hwnd,
#elif defined(__APPLE__)
                                 CAMetalLayer *metal_layer,
#else
                                 GHOST_TVulkanPlatformType platform,
                                 /* X11 */
                                 Window window,
                                 Display *display,
                                 /* Wayland */
                                 wl_surface *wayland_surface,
                                 wl_display *wayland_display,
                                 const GHOST_ContextVK_WindowInfo *wayland_window_info,
#endif
                                 int contextMajorVersion,
                                 int contextMinorVersion,
                                 int debug,
                                 const GHOST_GPUDevice &preferred_device)
    : GHOST_Context(stereoVisual),
#ifdef _WIN32
      m_hwnd(hwnd),
#elif defined(__APPLE__)
      m_metal_layer(metal_layer),
#else
      m_platform(platform),
      /* X11 */
      m_display(display),
      m_window(window),
      /* Wayland */
      m_wayland_surface(wayland_surface),
      m_wayland_display(wayland_display),
      m_wayland_window_info(wayland_window_info),
#endif
      m_context_major_version(contextMajorVersion),
      m_context_minor_version(contextMinorVersion),
      m_debug(debug),
      m_preferred_device(preferred_device),
      m_surface(VK_NULL_HANDLE),
      m_swapchain(VK_NULL_HANDLE),
      m_render_frame(0)
{
}

GHOST_ContextVK::~GHOST_ContextVK()
{
  if (vulkan_device.has_value()) {
    GHOST_DeviceVK &device_vk = *vulkan_device;
    device_vk.wait_idle();

    destroySwapchain();

    if (m_surface != VK_NULL_HANDLE) {
      vkDestroySurfaceKHR(device_vk.instance, m_surface, nullptr);
    }

    device_vk.users--;
    if (device_vk.users == 0) {
      vulkan_device.reset();
    }
  }
}

GHOST_TSuccess GHOST_ContextVK::swapBuffers()
{
  if (m_swapchain == VK_NULL_HANDLE) {
    return GHOST_kFailure;
  }

  assert(vulkan_device.has_value() && vulkan_device->device != VK_NULL_HANDLE);
  VkDevice device = vulkan_device->device;

  m_render_frame = (m_render_frame + 1) % m_image_count;
  GHOST_Frame &frame_data = m_frame_data[m_render_frame];
  /* Wait for the previous time this frame was used to be finished rendering. Presenting can still
   * happen in parallel, but acquiring needs can only happen when the frame acquire semaphore has
   * been signaled and waited for. */
  vkWaitForFences(device, 1, &frame_data.submission_fence, true, UINT64_MAX);
  frame_data.discard_pile.destroy(device);

#ifdef WITH_GHOST_WAYLAND
  /* Wayland doesn't provide a WSI with windowing capabilities, therefore cannot detect whether the
   * swap-chain needs to be recreated. But as a side effect we can recreate the swap chain before
   * presenting. */
  if (m_wayland_window_info) {
    const bool recreate_swapchain =
        ((m_wayland_window_info->size[0] !=
          std::max(m_render_extent.width, m_render_extent_min.width)) ||
         (m_wayland_window_info->size[1] !=
          std::max(m_render_extent.height, m_render_extent_min.height)));

    if (recreate_swapchain) {
      /* Swap-chain is out of date. Recreate swap-chain. */
      recreateSwapchain();
    }
  }
#endif

  /* Some platforms (NVIDIA/Wayland) can receive an out of date swapchain when acquiring the next
   * swapchain image. Other do it when calling vkQueuePresent. */
  VkResult acquire_result = VK_ERROR_OUT_OF_DATE_KHR;
  uint32_t image_index = 0;
  while (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
    acquire_result = vkAcquireNextImageKHR(device,
                                           m_swapchain,
                                           UINT64_MAX,
                                           frame_data.acquire_semaphore,
                                           VK_NULL_HANDLE,
                                           &image_index);
    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
      recreateSwapchain();
    }
  }

  GHOST_VulkanSwapChainData swap_chain_data;
  swap_chain_data.image = m_swapchain_images[image_index];
  swap_chain_data.surface_format = m_surface_format;
  swap_chain_data.extent = m_render_extent;
  swap_chain_data.submission_fence = frame_data.submission_fence;
  swap_chain_data.acquire_semaphore = frame_data.acquire_semaphore;
  swap_chain_data.present_semaphore = frame_data.present_semaphore;

  vkResetFences(device, 1, &frame_data.submission_fence);
  if (swap_buffers_pre_callback_) {
    swap_buffers_pre_callback_(&swap_chain_data);
  }

  VkPresentInfoKHR present_info = {};
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = &frame_data.present_semaphore;
  present_info.swapchainCount = 1;
  present_info.pSwapchains = &m_swapchain;
  present_info.pImageIndices = &image_index;
  present_info.pResults = nullptr;

  VkResult present_result = VK_SUCCESS;
  {
    std::scoped_lock lock(vulkan_device->queue_mutex);
    present_result = vkQueuePresentKHR(m_present_queue, &present_info);
  }
  if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR ||
      acquire_result == VK_SUBOPTIMAL_KHR)
  {
    /* Swap-chain is out of date. Recreate swap-chain and skip this frame. */
    recreateSwapchain();
    if (swap_buffers_post_callback_) {
      swap_buffers_post_callback_();
    }
    return GHOST_kSuccess;
  }
  if (present_result != VK_SUCCESS) {
    fprintf(stderr,
            "Error: Failed to present swap chain image : %s\n",
            vulkan_error_as_string(acquire_result));
  }

  if (swap_buffers_post_callback_) {
    swap_buffers_post_callback_();
  }

  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextVK::getVulkanSwapChainFormat(
    GHOST_VulkanSwapChainData *r_swap_chain_data)
{
  r_swap_chain_data->image = VK_NULL_HANDLE;
  r_swap_chain_data->surface_format = m_surface_format;
  r_swap_chain_data->extent = m_render_extent;

  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextVK::getVulkanHandles(GHOST_VulkanHandles &r_handles)
{
  r_handles = {
      VK_NULL_HANDLE, /* instance */
      VK_NULL_HANDLE, /* physical_device */
      VK_NULL_HANDLE, /* device */
      0,              /* queue_family */
      VK_NULL_HANDLE, /* queue */
      nullptr,        /* queue_mutex */
  };

  if (vulkan_device.has_value()) {
    r_handles = {
        vulkan_device->instance,
        vulkan_device->physical_device,
        vulkan_device->device,
        vulkan_device->generic_queue_family,
        m_graphic_queue,
        &vulkan_device->queue_mutex,
    };
  }

  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextVK::setVulkanSwapBuffersCallbacks(
    std::function<void(const GHOST_VulkanSwapChainData *)> swap_buffers_pre_callback,
    std::function<void(void)> swap_buffers_post_callback,
    std::function<void(GHOST_VulkanOpenXRData *)> openxr_acquire_framebuffer_image_callback,
    std::function<void(GHOST_VulkanOpenXRData *)> openxr_release_framebuffer_image_callback)
{
  swap_buffers_pre_callback_ = swap_buffers_pre_callback;
  swap_buffers_post_callback_ = swap_buffers_post_callback;
  openxr_acquire_framebuffer_image_callback_ = openxr_acquire_framebuffer_image_callback;
  openxr_release_framebuffer_image_callback_ = openxr_release_framebuffer_image_callback;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextVK::activateDrawingContext()
{
  active_context_ = this;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextVK::releaseDrawingContext()
{
  active_context_ = nullptr;
  return GHOST_kSuccess;
}

static vector<VkExtensionProperties> getExtensionsAvailable()
{
  uint32_t extension_count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);

  vector<VkExtensionProperties> extensions(extension_count);
  vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data());

  return extensions;
}

static bool checkExtensionSupport(const vector<VkExtensionProperties> &extensions_available,
                                  const char *extension_name)
{
  for (const auto &extension : extensions_available) {
    if (strcmp(extension_name, extension.extensionName) == 0) {
      return true;
    }
  }
  return false;
}

static void requireExtension(const vector<VkExtensionProperties> &extensions_available,
                             vector<const char *> &extensions_enabled,
                             const char *extension_name)
{
  if (checkExtensionSupport(extensions_available, extension_name)) {
    extensions_enabled.push_back(extension_name);
  }
  else {
    fprintf(stderr, "Error: %s not found.\n", extension_name);
  }
}

static GHOST_TSuccess selectPresentMode(VkPhysicalDevice device,
                                        VkSurfaceKHR surface,
                                        VkPresentModeKHR *r_presentMode)
{
  uint32_t present_count;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_count, nullptr);
  vector<VkPresentModeKHR> presents(present_count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_count, presents.data());
  /* MAILBOX is the lowest latency V-Sync enabled mode. We will use it if available as it fixes
   * some lag on NVIDIA/Intel GPUs. */
  /* TODO: select the correct presentation mode based on the actual being performed by the user.
   * When low latency is required (paint cursor) we should select mailbox, otherwise we can do FIFO
   * to reduce CPU/GPU usage.*/
  for (auto present_mode : presents) {
    if (present_mode == VK_PRESENT_MODE_MAILBOX_KHR) {
      *r_presentMode = present_mode;
      return GHOST_kSuccess;
    }
  }

  /* FIFO present mode is always available and we (should) prefer it as it will keep the main loop
   * running along the monitor refresh rate. Mailbox and FIFO relaxed can generate a lot of frames
   * that will never be displayed. */
  *r_presentMode = VK_PRESENT_MODE_FIFO_KHR;
  return GHOST_kSuccess;
}

/**
 * Select the surface format that we will use.
 *
 * We will select any 8bit UNORM surface.
 */
static bool selectSurfaceFormat(const VkPhysicalDevice physical_device,
                                const VkSurfaceKHR surface,
                                VkSurfaceFormatKHR &r_surfaceFormat)
{
  uint32_t format_count;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, nullptr);
  vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, formats.data());

  for (const VkSurfaceFormatKHR &format : formats) {
    if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
        format.format == VK_FORMAT_R8G8B8A8_UNORM)
    {
      r_surfaceFormat = format;
      return true;
    }
  }

  for (const VkSurfaceFormatKHR &format : formats) {
    if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
        format.format == VK_FORMAT_B8G8R8A8_UNORM)
    {
      r_surfaceFormat = format;
      return true;
    }
  }

  return false;
}

GHOST_TSuccess GHOST_ContextVK::recreateSwapchain()
{
  assert(vulkan_device.has_value() && vulkan_device->device != VK_NULL_HANDLE);

  VkPhysicalDevice physical_device = vulkan_device->physical_device;

  m_surface_format = {};
  if (!selectSurfaceFormat(physical_device, m_surface, m_surface_format)) {
    return GHOST_kFailure;
  }

  VkPresentModeKHR present_mode;
  if (!selectPresentMode(physical_device, m_surface, &present_mode)) {
    return GHOST_kFailure;
  }

  /* Query the surface capabilities for the given present mode on the surface. */
  VkSurfacePresentScalingCapabilitiesEXT vk_surface_present_scaling_capabilities = {
      VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT,
  };
  VkSurfaceCapabilities2KHR vk_surface_capabilities = {
      VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
      &vk_surface_present_scaling_capabilities,
  };
  VkSurfacePresentModeEXT vk_surface_present_mode = {
      VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT, nullptr, present_mode};
  VkPhysicalDeviceSurfaceInfo2KHR vk_physical_device_surface_info = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, &vk_surface_present_mode, m_surface};
  VkSurfaceCapabilitiesKHR capabilities = {};

  if (vulkan_device->use_vk_ext_swapchain_maintenance_1) {
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilities2KHR(
        physical_device, &vk_physical_device_surface_info, &vk_surface_capabilities));
    capabilities = vk_surface_capabilities.surfaceCapabilities;
  }
  else {
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, m_surface, &capabilities);
  }

  m_render_extent = capabilities.currentExtent;
  m_render_extent_min = capabilities.minImageExtent;
  if (m_render_extent.width == UINT32_MAX) {
    /* Window Manager is going to set the surface size based on the given size.
     * Choose something between minImageExtent and maxImageExtent. */
    int width = 0;
    int height = 0;

#ifdef WITH_GHOST_WAYLAND
    /* Wayland doesn't provide a windowing API via WSI. */
    if (m_wayland_window_info) {
      width = m_wayland_window_info->size[0];
      height = m_wayland_window_info->size[1];
    }
#endif

    if (width == 0 || height == 0) {
      width = 1280;
      height = 720;
    }

    m_render_extent.width = width;
    m_render_extent.height = height;

    if (capabilities.minImageExtent.width > m_render_extent.width) {
      m_render_extent.width = capabilities.minImageExtent.width;
    }
    if (capabilities.minImageExtent.height > m_render_extent.height) {
      m_render_extent.height = capabilities.minImageExtent.height;
    }

    if (vulkan_device->use_vk_ext_swapchain_maintenance_1) {
      if (vk_surface_present_scaling_capabilities.minScaledImageExtent.width >
          m_render_extent.width)
      {
        m_render_extent.width = vk_surface_present_scaling_capabilities.minScaledImageExtent.width;
      }
      if (vk_surface_present_scaling_capabilities.minScaledImageExtent.height >
          m_render_extent.height)
      {
        m_render_extent.height =
            vk_surface_present_scaling_capabilities.minScaledImageExtent.height;
      }
    }
  }

  /* Use double buffering when using FIFO. Increasing the number of images could stall when doing
   * actions that require low latency (paint cursor, UI resizing). MAILBOX prefers triple
   * buffering. */
  uint32_t image_count_requested = present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? 3 : 2;
  /* NOTE: maxImageCount == 0 means no limit. */
  if (capabilities.minImageCount != 0 && image_count_requested < capabilities.minImageCount) {
    image_count_requested = capabilities.minImageCount;
  }
  if (capabilities.maxImageCount != 0 && image_count_requested > capabilities.maxImageCount) {
    image_count_requested = capabilities.maxImageCount;
  }

  VkSwapchainPresentModesCreateInfoEXT vk_swapchain_present_modes = {
      VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT, nullptr, 1, &present_mode};
  VkSwapchainPresentScalingCreateInfoEXT vk_swapchain_present_scaling = {
      VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT,
      &vk_swapchain_present_modes,
      vk_surface_present_scaling_capabilities.supportedPresentScaling &
          VK_PRESENT_SCALING_STRETCH_BIT_EXT,
      vk_surface_present_scaling_capabilities.supportedPresentGravityX &
          VK_PRESENT_GRAVITY_CENTERED_BIT_EXT,
      vk_surface_present_scaling_capabilities.supportedPresentGravityY &
          VK_PRESENT_GRAVITY_CENTERED_BIT_EXT,
  };

  VkSwapchainKHR old_swapchain = m_swapchain;
  VkSwapchainCreateInfoKHR create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  if (vulkan_device->use_vk_ext_swapchain_maintenance_1) {
    create_info.pNext = &vk_swapchain_present_scaling;
    create_info.flags = VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_EXT;
  }
  create_info.surface = m_surface;
  create_info.minImageCount = image_count_requested;
  create_info.imageFormat = m_surface_format.format;
  create_info.imageColorSpace = m_surface_format.colorSpace;
  create_info.imageExtent = m_render_extent;
  create_info.imageArrayLayers = 1;
  create_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  create_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  create_info.presentMode = present_mode;
  create_info.clipped = VK_TRUE;
  create_info.oldSwapchain = old_swapchain;
  create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  create_info.queueFamilyIndexCount = 0;
  create_info.pQueueFamilyIndices = nullptr;

  VkDevice device = vulkan_device->device;
  VK_CHECK(vkCreateSwapchainKHR(device, &create_info, nullptr, &m_swapchain));

  /* image_count may not be what we requested! Getter for final value. */
  uint32_t actual_image_count = 0;
  vkGetSwapchainImagesKHR(device, m_swapchain, &actual_image_count, nullptr);
  m_swapchain_images.resize(actual_image_count);
  vkGetSwapchainImagesKHR(device, m_swapchain, &actual_image_count, m_swapchain_images.data());
  /* Construct new semaphores. It can be that image_count is larger than previously. We only need
   * to fill in where the handle is `VK_NULL_HANDLE`. */
  const VkSemaphoreCreateInfo vk_semaphore_create_info = {
      VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
  const VkFenceCreateInfo vk_fence_create_info = {
      VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
  if (actual_image_count > m_frame_data.size()) {
    m_frame_data.resize(actual_image_count);
  }
  for (int index = 0; index < m_frame_data.size(); index++) {
    GHOST_Frame &frame_data = m_frame_data[index];
    if (frame_data.acquire_semaphore == VK_NULL_HANDLE) {
      VK_CHECK(vkCreateSemaphore(
          device, &vk_semaphore_create_info, nullptr, &frame_data.acquire_semaphore));
    }
    if (frame_data.present_semaphore == VK_NULL_HANDLE) {
      VK_CHECK(vkCreateSemaphore(
          device, &vk_semaphore_create_info, nullptr, &frame_data.present_semaphore));
    }
    if (frame_data.submission_fence == VK_NULL_HANDLE) {
      VK_CHECK(
          vkCreateFence(device, &vk_fence_create_info, nullptr, &frame_data.submission_fence));
    }
  }

  m_image_count = actual_image_count;
  if (old_swapchain) {
    m_frame_data[m_render_frame].discard_pile.swapchains.push_back(old_swapchain);
  }

  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextVK::destroySwapchain()
{
  assert(vulkan_device.has_value() && vulkan_device->device != VK_NULL_HANDLE);
  VkDevice device = vulkan_device->device;

  if (m_swapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device, m_swapchain, nullptr);
  }
  VK_CHECK(vkDeviceWaitIdle(device));
  for (GHOST_Frame &frame_data : m_frame_data) {
    frame_data.destroy(device);
  }
  m_frame_data.clear();

  return GHOST_kSuccess;
}

const char *GHOST_ContextVK::getPlatformSpecificSurfaceExtension() const
{
#ifdef _WIN32
  return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif defined(__APPLE__)
  return VK_EXT_METAL_SURFACE_EXTENSION_NAME;
#else /* UNIX/Linux */
  switch (m_platform) {
#  ifdef WITH_GHOST_X11
    case GHOST_kVulkanPlatformX11:
      return VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
      break;
#  endif
#  ifdef WITH_GHOST_WAYLAND
    case GHOST_kVulkanPlatformWayland:
      return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
      break;
#  endif
    case GHOST_kVulkanPlatformHeadless:
      break;
  }
#endif
  return nullptr;
}

GHOST_TSuccess GHOST_ContextVK::initializeDrawingContext()
{
#ifdef _WIN32
  const bool use_window_surface = (m_hwnd != nullptr);
#elif defined(__APPLE__)
  const bool use_window_surface = (m_metal_layer != nullptr);
#else /* UNIX/Linux */
  bool use_window_surface = false;
  switch (m_platform) {
#  ifdef WITH_GHOST_X11
    case GHOST_kVulkanPlatformX11:
      use_window_surface = (m_display != nullptr) && (m_window != (Window) nullptr);
      break;
#  endif
#  ifdef WITH_GHOST_WAYLAND
    case GHOST_kVulkanPlatformWayland:
      use_window_surface = (m_wayland_display != nullptr) && (m_wayland_surface != nullptr);
      break;
#  endif
    case GHOST_kVulkanPlatformHeadless:
      use_window_surface = false;
      break;
  }
#endif

  std::vector<VkExtensionProperties> extensions_available = getExtensionsAvailable();
  vector<const char *> required_device_extensions;
  vector<const char *> optional_device_extensions;
  vector<const char *> extensions_enabled;

  if (m_debug) {
    requireExtension(extensions_available, extensions_enabled, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  if (use_window_surface) {
    const char *native_surface_extension_name = getPlatformSpecificSurfaceExtension();
    requireExtension(extensions_available, extensions_enabled, VK_KHR_SURFACE_EXTENSION_NAME);
    requireExtension(extensions_available, extensions_enabled, native_surface_extension_name);

    /* Required instance extension dependency of VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME */
    if (contains_extension(extensions_available, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
        contains_extension(extensions_available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME))
    {
      requireExtension(
          extensions_available, extensions_enabled, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
      requireExtension(extensions_available,
                       extensions_enabled,
                       VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
      optional_device_extensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    }

    required_device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }

  /* External memory extensions. */
  required_device_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
#ifdef _WIN32
  /* Placeholder to add VK_KHR_external_memory_win32  */
#elif not defined(__APPLE__)
  optional_device_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
#endif

#ifdef __APPLE__
  optional_device_extensions.push_back(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
#else
  required_device_extensions.push_back(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
#endif
  optional_device_extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
  optional_device_extensions.push_back(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
  optional_device_extensions.push_back(VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME);
  optional_device_extensions.push_back(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
  optional_device_extensions.push_back(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
  optional_device_extensions.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);

  /* Enable MoltenVK required instance extensions. */
#ifdef __APPLE__
  requireExtension(
      extensions_available, extensions_enabled, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

  VkInstance instance = VK_NULL_HANDLE;
  if (!vulkan_device.has_value()) {
    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Blender";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "Blender";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_MAKE_VERSION(m_context_major_version, m_context_minor_version, 0);

    /* Create Instance */
    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = uint32_t(extensions_enabled.size());
    create_info.ppEnabledExtensionNames = extensions_enabled.data();

    /* VkValidationFeaturesEXT */
    VkValidationFeaturesEXT validationFeatures = {};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.enabledValidationFeatureCount = 1;

    VkValidationFeatureEnableEXT enabledValidationFeatures[1] = {
        VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT};
    validationFeatures.pEnabledValidationFeatures = enabledValidationFeatures;
    if (m_debug) {
      create_info.pNext = &validationFeatures;
    }

#ifdef __APPLE__
    create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    VK_CHECK(vkCreateInstance(&create_info, nullptr, &instance));
  }
  else {
    instance = vulkan_device->instance;
  }

  if (use_window_surface) {
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR surface_create_info = {};
    surface_create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_create_info.hinstance = GetModuleHandle(nullptr);
    surface_create_info.hwnd = m_hwnd;
    VK_CHECK(vkCreateWin32SurfaceKHR(instance, &surface_create_info, nullptr, &m_surface));
#elif defined(__APPLE__)
    VkMetalSurfaceCreateInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    info.pNext = nullptr;
    info.flags = 0;
    info.pLayer = m_metal_layer;
    VK_CHECK(vkCreateMetalSurfaceEXT(instance, &info, nullptr, &m_surface));
#else
    switch (m_platform) {
#  ifdef WITH_GHOST_X11
      case GHOST_kVulkanPlatformX11: {
        VkXlibSurfaceCreateInfoKHR surface_create_info = {};
        surface_create_info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
        surface_create_info.dpy = m_display;
        surface_create_info.window = m_window;
        VK_CHECK(vkCreateXlibSurfaceKHR(instance, &surface_create_info, nullptr, &m_surface));
        break;
      }
#  endif
#  ifdef WITH_GHOST_WAYLAND
      case GHOST_kVulkanPlatformWayland: {
        VkWaylandSurfaceCreateInfoKHR surface_create_info = {};
        surface_create_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        surface_create_info.display = m_wayland_display;
        surface_create_info.surface = m_wayland_surface;
        VK_CHECK(vkCreateWaylandSurfaceKHR(instance, &surface_create_info, nullptr, &m_surface));
        break;
      }
#  endif
      case GHOST_kVulkanPlatformHeadless: {
        m_surface = VK_NULL_HANDLE;
        break;
      }
    }

#endif
  }

  if (!ensure_vulkan_device(instance, m_surface, m_preferred_device, required_device_extensions)) {
    return GHOST_kFailure;
  }

  vulkan_device->users++;
  vulkan_device->ensure_device(required_device_extensions, optional_device_extensions);

  vkGetDeviceQueue(
      vulkan_device->device, vulkan_device->generic_queue_family, 0, &m_graphic_queue);

  if (use_window_surface) {
    vkGetDeviceQueue(
        vulkan_device->device, vulkan_device->generic_queue_family, 0, &m_present_queue);
    recreateSwapchain();
  }

  active_context_ = this;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextVK::releaseNativeHandles()
{
  return GHOST_kSuccess;
}
