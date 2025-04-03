/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_node.hh"

#include "NOD_geometry_nodes_closure_fwd.hh"
#include "NOD_socket_interface_key.hh"

#include "BLI_resource_scope.hh"

#include "FN_lazy_function.hh"

namespace blender::nodes {

/** Describes the names and types of the inputs and outputs of a closure. */
class ClosureSignature {
 public:
  struct Item {
    SocketInterfaceKey key;
    const bke::bNodeSocketType *type = nullptr;
  };

  Vector<Item> inputs;
  Vector<Item> outputs;

  std::optional<int> find_input_index(const SocketInterfaceKey &key) const;
  std::optional<int> find_output_index(const SocketInterfaceKey &key) const;
};

/**
 * Describes the meaning of the various inputs and outputs of the lazy-function that's contained
 * in the closure.
 */
struct ClosureFunctionIndices {
  struct {
    IndexRange main;
    /** A boolean input for each output indicating whether that output is used. */
    IndexRange output_usages;
    /**
     * A #GeometryNodesReferenceSet input for a subset of the outputs. This is used to tell the
     * closure which attributes it has to propagate to the outputs.
     *
     * Main output index -> input lf socket index.
     */
    Map<int, int> output_data_reference_sets;
  } inputs;
  struct {
    IndexRange main;
    /** A boolean output for each input indicating whether that input is used. */
    IndexRange input_usages;
  } outputs;
};

/**
 * A closure is like a node group that is passed around as a value. It's typically evaluated using
 * the Evaluate Closure node.
 *
 * Internally, a closure is a lazy-function. So the inputs that are passed to the closure are
 * requested lazily. It's *not* yet supported to request the potentially captured values from the
 * Closure Zone lazily.
 */
class Closure : public ImplicitSharingMixin {
 private:
  std::shared_ptr<ClosureSignature> signature_;
  /**
   * When building complex lazy-functions, e.g. from Geometry Nodes, one often has to allocate
   * various additional resources (e.g. the lazy-functions for the individual nodes). Using
   * #ResourceScope provides a simple way to pass ownership of all these additional resources to
   * the Closure.
   */
  std::unique_ptr<ResourceScope> scope_;
  const fn::lazy_function::LazyFunction &function_;
  ClosureFunctionIndices indices_;
  Vector<const void *> default_input_values_;

 public:
  Closure(std::shared_ptr<ClosureSignature> signature,
          std::unique_ptr<ResourceScope> scope,
          const fn::lazy_function::LazyFunction &function,
          ClosureFunctionIndices indices,
          Vector<const void *> default_input_values)
      : signature_(signature),
        scope_(std::move(scope)),
        function_(function),
        indices_(indices),
        default_input_values_(std::move(default_input_values))
  {
  }

  const ClosureSignature &signature() const
  {
    return *signature_;
  }

  const ClosureFunctionIndices &indices() const
  {
    return indices_;
  }

  const fn::lazy_function::LazyFunction &function() const
  {
    return function_;
  }

  const void *default_input_value(const int index) const
  {
    return default_input_values_[index];
  }

  void delete_self() override
  {
    MEM_delete(this);
  }
};

}  // namespace blender::nodes
