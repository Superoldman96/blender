/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
  vec2 output_size = vec2(imageSize(output_img));

  vec2 coordinates = (vec2(texel) + vec2(0.5)) / output_size;

  vec4 accumulated_color = vec4(0.0);
  for (int i = 0; i < number_of_motion_blur_samples; i++) {
    mat3 homography_matrix = mat3(homography_matrices[i]);

    vec3 transformed_coordinates = homography_matrix * vec3(coordinates, 1.0);
    vec2 projected_coordinates = transformed_coordinates.xy / transformed_coordinates.z;

    /* The derivatives of the projected coordinates with respect to x and y are the first and
     * second columns respectively, divided by the z projection factor as can be shown by
     * differentiating the above matrix multiplication with respect to x and y. Divide by the
     * output size since textureGrad assumes derivatives with respect to texel coordinates. */
    vec2 x_gradient = (homography_matrix[0].xy / transformed_coordinates.z) / output_size.x;
    vec2 y_gradient = (homography_matrix[1].xy / transformed_coordinates.z) / output_size.y;

    vec4 sampled_color = textureGrad(input_tx, projected_coordinates, x_gradient, y_gradient);
    accumulated_color += sampled_color;
  }

  accumulated_color /= number_of_motion_blur_samples;

  /* Premultiply the mask value as an alpha. */
  vec4 plane_color = accumulated_color * texture_load(mask_tx, texel).x;

  imageStore(output_img, texel, plane_color);
}
