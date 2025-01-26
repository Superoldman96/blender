/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_multi_value_map.hh"
#include "BLI_span.hh"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"

namespace blender::geometry {

const MultiValueMap<bke::GeometryComponent::Type, bke::AttrDomain> &
components_supported_reordering();

Mesh *reorder_mesh(const Mesh &src_mesh,
                   Span<int> old_by_new_map,
                   bke::AttrDomain domain,
                   const bke::AttributeFilter &attribute_filter);

PointCloud *reorder_points(const PointCloud &src_pointcloud,
                           Span<int> old_by_new_map,
                           const bke::AttributeFilter &attribute_filter);

bke::CurvesGeometry reorder_curves_geometry(const bke::CurvesGeometry &src_curves,
                                            Span<int> old_by_new_map,
                                            const bke::AttributeFilter &attribute_filter);

Curves *reorder_curves(const Curves &src_curves,
                       Span<int> old_by_new_map,
                       const bke::AttributeFilter &attribute_filter);

bke::Instances *reorder_instaces(const bke::Instances &src_instances,
                                 Span<int> old_by_new_map,
                                 const bke::AttributeFilter &attribute_filter);

bke::GeometryComponentPtr reordered_component(const bke::GeometryComponent &src_component,
                                              Span<int> old_by_new_map,
                                              bke::AttrDomain domain,
                                              const bke::AttributeFilter &attribute_filter);

};  // namespace blender::geometry
