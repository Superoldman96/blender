/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_matrix.hh"

#include "node_function_util.hh"

namespace blender::nodes::node_fn_project_point_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.is_function_node();
  b.add_input<decl::Vector>("Vector").subtype(PROP_XYZ);
  b.add_output<decl::Vector>("Vector").subtype(PROP_XYZ).align_with_previous();
  b.add_input<decl::Matrix>("Transform");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto fn = mf::build::SI2_SO<float3, float4x4, float3>(
      "Project Point",
      [](float3 point, float4x4 matrix) { return math::project_point(matrix, point); });
  builder.set_matching_fn(fn);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  fn_node_type_base(&ntype, "FunctionNodeProjectPoint", FN_NODE_PROJECT_POINT);
  ntype.ui_name = "Project Point";
  ntype.ui_description =
      "Project a point using a matrix, using location, rotation, scale, and perspective divide";
  ntype.enum_name_legacy = "PROJECT_POINT";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_project_point_cc
