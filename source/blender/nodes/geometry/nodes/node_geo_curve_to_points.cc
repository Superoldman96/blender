/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_matrix.hh"

#include "DNA_pointcloud_types.h"

#include "BKE_customdata.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"
#include "BKE_pointcloud.hh"

#include "GEO_join_geometries.hh"
#include "GEO_resample_curves.hh"

#include "NOD_rna_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_to_points_cc {

NODE_STORAGE_FUNCS(NodeGeometryCurveToPoints)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curve").supported_type(
      {GeometryComponent::Type::Curve, GeometryComponent::Type::GreasePencil});
  auto &count = b.add_input<decl::Int>("Count")
                    .default_value(10)
                    .min(2)
                    .max(100000)
                    .field_on_all()
                    .make_available([](bNode &node) {
                      node_storage(node).mode = GEO_NODE_CURVE_RESAMPLE_COUNT;
                    });
  auto &length = b.add_input<decl::Float>("Length")
                     .default_value(0.1f)
                     .min(0.001f)
                     .subtype(PROP_DISTANCE)
                     .make_available([](bNode &node) {
                       node_storage(node).mode = GEO_NODE_CURVE_RESAMPLE_LENGTH;
                     });
  b.add_output<decl::Geometry>("Points").propagate_all();
  b.add_output<decl::Vector>("Tangent").field_on_all();
  b.add_output<decl::Vector>("Normal").field_on_all();
  b.add_output<decl::Rotation>("Rotation").field_on_all();

  const bNode *node = b.node_or_null();
  if (node != nullptr) {
    const NodeGeometryCurveToPoints &storage = node_storage(*node);
    const GeometryNodeCurveResampleMode mode = GeometryNodeCurveResampleMode(storage.mode);

    count.available(mode == GEO_NODE_CURVE_RESAMPLE_COUNT);
    length.available(mode == GEO_NODE_CURVE_RESAMPLE_LENGTH);
  }
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryCurveToPoints *data = MEM_callocN<NodeGeometryCurveToPoints>(__func__);

  data->mode = GEO_NODE_CURVE_RESAMPLE_COUNT;
  node->storage = data;
}

static void fill_rotation_attribute(const Span<float3> tangents,
                                    const Span<float3> normals,
                                    MutableSpan<math::Quaternion> rotations)
{
  threading::parallel_for(IndexRange(rotations.size()), 512, [&](IndexRange range) {
    for (const int i : range) {
      rotations[i] = math::to_quaternion(
          math::from_orthonormal_axes<float4x4>(normals[i], tangents[i]));
    }
  });
}

static void copy_curve_domain_attributes(const AttributeAccessor curve_attributes,
                                         MutableAttributeAccessor point_attributes)
{
  curve_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.is_builtin) {
      return;
    }
    if (iter.domain != AttrDomain::Curve) {
      return;
    }
    if (iter.data_type == CD_PROP_STRING) {
      return;
    }
    point_attributes.add(iter.name,
                         AttrDomain::Point,
                         iter.data_type,
                         bke::AttributeInitVArray(*iter.get(AttrDomain::Point)));
  });
}

static PointCloud *pointcloud_from_curves(bke::CurvesGeometry curves,
                                          const std::optional<StringRef> &tangent_id,
                                          const std::optional<StringRef> &normal_id,
                                          const std::optional<StringRef> &rotation_id)
{
  PointCloud *pointcloud = BKE_pointcloud_new_nomain(0);
  pointcloud->totpoint = curves.points_num();

  if (rotation_id) {
    MutableAttributeAccessor attributes = curves.attributes_for_write();
    const VArraySpan tangents = *attributes.lookup<float3>(*tangent_id, AttrDomain::Point);
    const VArraySpan normals = *attributes.lookup<float3>(*normal_id, AttrDomain::Point);
    SpanAttributeWriter<math::Quaternion> rotations =
        attributes.lookup_or_add_for_write_only_span<math::Quaternion>(*rotation_id,
                                                                       AttrDomain::Point);
    fill_rotation_attribute(tangents, normals, rotations.span);
    rotations.finish();
  }

  /* Move the curve point custom data to the pointcloud, to avoid any copying. */
  CustomData_free(&pointcloud->pdata);
  pointcloud->pdata = curves.point_data;
  CustomData_reset(&curves.point_data);

  copy_curve_domain_attributes(curves.attributes(), pointcloud->attributes_for_write());

  return pointcloud;
}

static void curve_to_points(GeometrySet &geometry_set,
                            GeoNodeExecParams params,
                            const GeometryNodeCurveResampleMode mode,
                            geometry::ResampleCurvesOutputAttributeIDs resample_attributes,
                            const std::optional<StringRef> &rotation_anonymous_id)
{
  switch (mode) {
    case GEO_NODE_CURVE_RESAMPLE_COUNT: {
      Field<int> count = params.extract_input<Field<int>>("Count");
      geometry_set.modify_geometry_sets([&](GeometrySet &geometry) {
        if (const Curves *src_curves_id = geometry.get_curves()) {
          const bke::CurvesGeometry &src_curves = src_curves_id->geometry.wrap();
          const bke::CurvesFieldContext field_context{*src_curves_id, AttrDomain::Curve};
          bke::CurvesGeometry dst_curves = geometry::resample_to_count(
              src_curves,
              field_context,
              fn::make_constant_field<bool>(true),
              count,
              resample_attributes);
          PointCloud *pointcloud = pointcloud_from_curves(std::move(dst_curves),
                                                          resample_attributes.tangent_id,
                                                          resample_attributes.normal_id,
                                                          rotation_anonymous_id);
          geometry.remove_geometry_during_modify();
          geometry.replace_pointcloud(pointcloud);
        }
      });
      break;
    }
    case GEO_NODE_CURVE_RESAMPLE_LENGTH: {
      Field<float> length = params.extract_input<Field<float>>("Length");
      geometry_set.modify_geometry_sets([&](GeometrySet &geometry) {
        if (const Curves *src_curves_id = geometry.get_curves()) {
          const bke::CurvesGeometry &src_curves = src_curves_id->geometry.wrap();
          const bke::CurvesFieldContext field_context{*src_curves_id, AttrDomain::Curve};
          bke::CurvesGeometry dst_curves = geometry::resample_to_length(
              src_curves,
              field_context,
              fn::make_constant_field<bool>(true),
              length,
              resample_attributes);
          PointCloud *pointcloud = pointcloud_from_curves(std::move(dst_curves),
                                                          resample_attributes.tangent_id,
                                                          resample_attributes.normal_id,
                                                          rotation_anonymous_id);
          geometry.remove_geometry_during_modify();
          geometry.replace_pointcloud(pointcloud);
        }
      });
      break;
    }
    case GEO_NODE_CURVE_RESAMPLE_EVALUATED: {
      geometry_set.modify_geometry_sets([&](GeometrySet &geometry) {
        if (const Curves *src_curves_id = geometry.get_curves()) {
          const bke::CurvesGeometry &src_curves = src_curves_id->geometry.wrap();
          const bke::CurvesFieldContext field_context{*src_curves_id, AttrDomain::Curve};
          bke::CurvesGeometry dst_curves = geometry::resample_to_evaluated(
              src_curves, field_context, fn::make_constant_field<bool>(true), resample_attributes);
          PointCloud *pointcloud = pointcloud_from_curves(std::move(dst_curves),
                                                          resample_attributes.tangent_id,
                                                          resample_attributes.normal_id,
                                                          rotation_anonymous_id);
          geometry.remove_geometry_during_modify();
          geometry.replace_pointcloud(pointcloud);
        }
      });
      break;
    }
  }
}

static void grease_pencil_to_points(GeometrySet &geometry_set,
                                    GeoNodeExecParams params,
                                    const GeometryNodeCurveResampleMode mode,
                                    geometry::ResampleCurvesOutputAttributeIDs resample_attributes,
                                    const std::optional<StringRef> &rotation_anonymous_id,
                                    const AttributeFilter &attribute_filter)
{
  Field<int> count;
  Field<float> length;

  switch (mode) {
    case GEO_NODE_CURVE_RESAMPLE_COUNT:
      count = params.extract_input<Field<int>>("Count");
      break;
    case GEO_NODE_CURVE_RESAMPLE_LENGTH:
      length = params.extract_input<Field<float>>("Length");
      break;
    case GEO_NODE_CURVE_RESAMPLE_EVALUATED:
      break;
  }

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry) {
    using namespace blender::bke::greasepencil;
    if (geometry.has_grease_pencil()) {
      const GreasePencil &grease_pencil = *geometry.get_grease_pencil();
      Vector<PointCloud *> pointcloud_by_layer(grease_pencil.layers().size(), nullptr);
      for (const int layer_index : grease_pencil.layers().index_range()) {
        const Drawing *drawing = grease_pencil.get_eval_drawing(grease_pencil.layer(layer_index));
        if (drawing == nullptr) {
          continue;
        }
        const bke::CurvesGeometry &src_curves = drawing->strokes();
        bke::GreasePencilLayerFieldContext field_context(
            grease_pencil, AttrDomain::Curve, layer_index);

        bke::CurvesGeometry dst_curves;
        switch (mode) {
          case GEO_NODE_CURVE_RESAMPLE_COUNT: {
            dst_curves = geometry::resample_to_count(src_curves,
                                                     field_context,
                                                     fn::make_constant_field<bool>(true),
                                                     count,
                                                     resample_attributes);
            break;
          }
          case GEO_NODE_CURVE_RESAMPLE_LENGTH: {
            dst_curves = geometry::resample_to_length(src_curves,
                                                      field_context,
                                                      fn::make_constant_field<bool>(true),
                                                      length,
                                                      resample_attributes);
            break;
          }
          case GEO_NODE_CURVE_RESAMPLE_EVALUATED: {
            dst_curves = geometry::resample_to_evaluated(src_curves,
                                                         field_context,
                                                         fn::make_constant_field<bool>(true),
                                                         resample_attributes);
            break;
          }
        }
        pointcloud_by_layer[layer_index] = pointcloud_from_curves(std::move(dst_curves),
                                                                  resample_attributes.tangent_id,
                                                                  resample_attributes.normal_id,
                                                                  rotation_anonymous_id);
      }
      if (!pointcloud_by_layer.is_empty()) {
        bke::Instances *instances = new bke::Instances();
        for (PointCloud *pointcloud : pointcloud_by_layer) {
          if (!pointcloud) {
            /* Add an empty reference so the number of layers and instances match.
             * This makes it easy to reconstruct the layers afterwards and keep their
             * attributes. */
            const int handle = instances->add_reference(bke::InstanceReference());
            instances->add_instance(handle, float4x4::identity());
            continue;
          }
          GeometrySet temp_set = GeometrySet::from_pointcloud(pointcloud);
          const int handle = instances->add_reference(bke::InstanceReference{temp_set});
          instances->add_instance(handle, float4x4::identity());
        }

        bke::copy_attributes(geometry.get_grease_pencil()->attributes(),
                             bke::AttrDomain::Layer,
                             bke::AttrDomain::Instance,
                             attribute_filter,
                             instances->attributes_for_write());
        InstancesComponent &dst_component = geometry.get_component_for_write<InstancesComponent>();
        GeometrySet new_instances = geometry::join_geometries(
            {GeometrySet::from_instances(dst_component.release()),
             GeometrySet::from_instances(instances)},
            attribute_filter);
        dst_component.replace(
            new_instances.get_component_for_write<InstancesComponent>().release());
      }
    }
  });
  geometry_set.replace_grease_pencil(nullptr);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeGeometryCurveToPoints &storage = node_storage(params.node());
  const GeometryNodeCurveResampleMode mode = (GeometryNodeCurveResampleMode)storage.mode;
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curve");

  GeometryComponentEditData::remember_deformed_positions_if_necessary(geometry_set);

  std::optional<std::string> rotation_anonymous_id =
      params.get_output_anonymous_attribute_id_if_needed("Rotation");
  const bool need_tangent_and_normal = bool(rotation_anonymous_id);
  std::optional<std::string> tangent_anonymous_id =
      params.get_output_anonymous_attribute_id_if_needed("Tangent", need_tangent_and_normal);
  std::optional<std::string> normal_anonymous_id =
      params.get_output_anonymous_attribute_id_if_needed("Normal", need_tangent_and_normal);

  geometry::ResampleCurvesOutputAttributeIDs resample_attributes;
  resample_attributes.tangent_id = tangent_anonymous_id;
  resample_attributes.normal_id = normal_anonymous_id;
  const NodeAttributeFilter &attribute_filter = params.get_attribute_filter("Points");

  if (geometry_set.has_curves()) {
    curve_to_points(geometry_set, params, mode, resample_attributes, rotation_anonymous_id);
  }
  if (geometry_set.has_grease_pencil()) {
    grease_pencil_to_points(
        geometry_set, params, mode, resample_attributes, rotation_anonymous_id, attribute_filter);
  }

  params.set_output("Points", std::move(geometry_set));
}

static void node_rna(StructRNA *srna)
{
  static EnumPropertyItem mode_items[] = {
      {GEO_NODE_CURVE_RESAMPLE_EVALUATED,
       "EVALUATED",
       0,
       "Evaluated",
       "Create points from the curve's evaluated points, based on the resolution attribute for "
       "NURBS and Bézier splines"},
      {GEO_NODE_CURVE_RESAMPLE_COUNT,
       "COUNT",
       0,
       "Count",
       "Sample each spline by evenly distributing the specified number of points"},
      {GEO_NODE_CURVE_RESAMPLE_LENGTH,
       "LENGTH",
       0,
       "Length",
       "Sample each spline by splitting it into segments with the specified length"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_node_enum(srna,
                    "mode",
                    "Mode",
                    "How to generate points from the input curve",
                    mode_items,
                    NOD_storage_enum_accessors(mode),
                    GEO_NODE_CURVE_RESAMPLE_COUNT);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCurveToPoints", GEO_NODE_CURVE_TO_POINTS);
  ntype.ui_name = "Curve to Points";
  ntype.ui_description = "Generate a point cloud by sampling positions along curves";
  ntype.enum_name_legacy = "CURVE_TO_POINTS";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  blender::bke::node_type_storage(
      ntype, "NodeGeometryCurveToPoints", node_free_standard_storage, node_copy_standard_storage);
  ntype.initfunc = node_init;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curve_to_points_cc
