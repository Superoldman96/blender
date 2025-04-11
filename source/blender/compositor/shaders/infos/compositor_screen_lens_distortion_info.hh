/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_screen_lens_distortion_shared)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float3, chromatic_distortion)
PUSH_CONSTANT(float, scale)
SAMPLER(0, FLOAT_2D, input_tx)
IMAGE(0, GPU_RGBA16F, WRITE, FLOAT_2D, output_img)
COMPUTE_SOURCE("compositor_screen_lens_distortion.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_screen_lens_distortion)
ADDITIONAL_INFO(compositor_screen_lens_distortion_shared)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_screen_lens_distortion_jitter)
ADDITIONAL_INFO(compositor_screen_lens_distortion_shared)
DEFINE("JITTER")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
