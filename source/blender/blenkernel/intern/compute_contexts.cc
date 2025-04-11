/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_modifier_types.h"
#include "DNA_node_types.h"

#include "BKE_compute_context_cache.hh"
#include "BKE_compute_contexts.hh"

#include <ostream>

namespace blender::bke {

ModifierComputeContext::ModifierComputeContext(const ComputeContext *parent,
                                               const NodesModifierData &nmd)
    : ModifierComputeContext(parent, nmd.modifier.name)
{
  nmd_ = &nmd;
}

ModifierComputeContext::ModifierComputeContext(const ComputeContext *parent,
                                               std::string modifier_name)
    : ComputeContext(s_static_type, parent), modifier_name_(std::move(modifier_name))
{
  hash_.mix_in(s_static_type, strlen(s_static_type));
  hash_.mix_in(modifier_name_.data(), modifier_name_.size());
}

void ModifierComputeContext::print_current_in_line(std::ostream &stream) const
{
  stream << "Modifier: " << modifier_name_;
}

GroupNodeComputeContext::GroupNodeComputeContext(
    const ComputeContext *parent,
    const int32_t node_id,
    const std::optional<ComputeContextHash> &cached_hash)
    : ComputeContext(s_static_type, parent), node_id_(node_id)
{
  if (cached_hash.has_value()) {
    hash_ = *cached_hash;
  }
  else {
    /* Mix static type and node id into a single buffer so that only a single call to #mix_in is
     * necessary. */
    const int type_size = strlen(s_static_type);
    const int buffer_size = type_size + 1 + sizeof(int32_t);
    DynamicStackBuffer<64, 8> buffer_owner(buffer_size, 8);
    char *buffer = static_cast<char *>(buffer_owner.buffer());
    memcpy(buffer, s_static_type, type_size + 1);
    memcpy(buffer + type_size + 1, &node_id_, sizeof(int32_t));
    hash_.mix_in(buffer, buffer_size);
  }
}

GroupNodeComputeContext::GroupNodeComputeContext(
    const ComputeContext *parent,
    const bNode &caller_group_node,
    const bNodeTree &caller_tree,
    const std::optional<ComputeContextHash> &cached_hash)
    : GroupNodeComputeContext(parent, caller_group_node.identifier, cached_hash)
{
  caller_group_node_ = &caller_group_node;
  caller_tree_ = &caller_tree;
}

void GroupNodeComputeContext::print_current_in_line(std::ostream &stream) const
{
  if (caller_group_node_ != nullptr) {
    stream << "Node: " << caller_group_node_->name;
    return;
  }
}

SimulationZoneComputeContext::SimulationZoneComputeContext(const ComputeContext *parent,
                                                           const int32_t output_node_id)
    : ComputeContext(s_static_type, parent), output_node_id_(output_node_id)
{
  /* Mix static type and node id into a single buffer so that only a single call to #mix_in is
   * necessary. */
  const int type_size = strlen(s_static_type);
  const int buffer_size = type_size + 1 + sizeof(int32_t);
  DynamicStackBuffer<64, 8> buffer_owner(buffer_size, 8);
  char *buffer = static_cast<char *>(buffer_owner.buffer());
  memcpy(buffer, s_static_type, type_size + 1);
  memcpy(buffer + type_size + 1, &output_node_id_, sizeof(int32_t));
  hash_.mix_in(buffer, buffer_size);
}

SimulationZoneComputeContext::SimulationZoneComputeContext(const ComputeContext *parent,
                                                           const bNode &node)
    : SimulationZoneComputeContext(parent, node.identifier)
{
}

void SimulationZoneComputeContext::print_current_in_line(std::ostream &stream) const
{
  stream << "Simulation Zone ID: " << output_node_id_;
}

RepeatZoneComputeContext::RepeatZoneComputeContext(const ComputeContext *parent,
                                                   const int32_t output_node_id,
                                                   const int iteration)
    : ComputeContext(s_static_type, parent), output_node_id_(output_node_id), iteration_(iteration)
{
  /* Mix static type and node id into a single buffer so that only a single call to #mix_in is
   * necessary. */
  const int type_size = strlen(s_static_type);
  const int buffer_size = type_size + 1 + sizeof(int32_t) + sizeof(int);
  DynamicStackBuffer<64, 8> buffer_owner(buffer_size, 8);
  char *buffer = static_cast<char *>(buffer_owner.buffer());
  memcpy(buffer, s_static_type, type_size + 1);
  memcpy(buffer + type_size + 1, &output_node_id_, sizeof(int32_t));
  memcpy(buffer + type_size + 1 + sizeof(int32_t), &iteration_, sizeof(int));
  hash_.mix_in(buffer, buffer_size);
}

RepeatZoneComputeContext::RepeatZoneComputeContext(const ComputeContext *parent,
                                                   const bNode &node,
                                                   const int iteration)
    : RepeatZoneComputeContext(parent, node.identifier, iteration)
{
}

void RepeatZoneComputeContext::print_current_in_line(std::ostream &stream) const
{
  stream << "Repeat Zone ID: " << output_node_id_;
}

ForeachGeometryElementZoneComputeContext::ForeachGeometryElementZoneComputeContext(
    const ComputeContext *parent, const int32_t output_node_id, const int index)
    : ComputeContext(s_static_type, parent), output_node_id_(output_node_id), index_(index)
{
  /* Mix static type and node id into a single buffer so that only a single call to #mix_in is
   * necessary. */
  const int type_size = strlen(s_static_type);
  const int buffer_size = type_size + 1 + sizeof(int32_t) + sizeof(int);
  DynamicStackBuffer<64, 8> buffer_owner(buffer_size, 8);
  char *buffer = static_cast<char *>(buffer_owner.buffer());
  memcpy(buffer, s_static_type, type_size + 1);
  memcpy(buffer + type_size + 1, &output_node_id_, sizeof(int32_t));
  memcpy(buffer + type_size + 1 + sizeof(int32_t), &index_, sizeof(int));
  hash_.mix_in(buffer, buffer_size);
}

ForeachGeometryElementZoneComputeContext::ForeachGeometryElementZoneComputeContext(
    const ComputeContext *parent, const bNode &node, const int index)
    : ForeachGeometryElementZoneComputeContext(parent, node.identifier, index)
{
}

void ForeachGeometryElementZoneComputeContext::print_current_in_line(std::ostream &stream) const
{
  stream << "Foreach Geometry Element Zone ID: " << output_node_id_;
}

EvaluateClosureComputeContext::EvaluateClosureComputeContext(const ComputeContext *parent,
                                                             const int32_t node_id)
    : ComputeContext(s_static_type, parent), node_id_(node_id)
{
  /* Mix static type and node id into a single buffer so that only a single call to #mix_in is
   * necessary. */
  const int type_size = strlen(s_static_type);
  const int buffer_size = type_size + 1 + sizeof(int32_t);
  DynamicStackBuffer<64, 8> buffer_owner(buffer_size, 8);
  char *buffer = static_cast<char *>(buffer_owner.buffer());
  memcpy(buffer, s_static_type, type_size + 1);
  memcpy(buffer + type_size + 1, &node_id_, sizeof(int32_t));
  hash_.mix_in(buffer, buffer_size);
}

EvaluateClosureComputeContext::EvaluateClosureComputeContext(
    const ComputeContext *parent,
    const bNode &node,
    const std::optional<nodes::ClosureSourceLocation> &closure_source_location)
    : EvaluateClosureComputeContext(parent, node.identifier)
{
  evaluate_node_ = &node;
  closure_source_location_ = closure_source_location;
}

void EvaluateClosureComputeContext::print_current_in_line(std::ostream &stream) const
{
  if (evaluate_node_ != nullptr) {
    stream << "Evaluate Closure: " << evaluate_node_->name;
    return;
  }
}

OperatorComputeContext::OperatorComputeContext() : OperatorComputeContext(nullptr) {}

OperatorComputeContext::OperatorComputeContext(const ComputeContext *parent)
    : ComputeContext(s_static_type, parent)
{
  hash_.mix_in(s_static_type, strlen(s_static_type));
}

OperatorComputeContext::OperatorComputeContext(const ComputeContext *parent, const bNodeTree &tree)
    : OperatorComputeContext(parent)
{
  tree_ = &tree;
}

void OperatorComputeContext::print_current_in_line(std::ostream &stream) const
{
  stream << "Operator";
}

const ModifierComputeContext &ComputeContextCache::for_modifier(const ComputeContext *parent,
                                                                const NodesModifierData &nmd)
{
  return *modifier_contexts_cache_.lookup_or_add_cb(
      std::pair{parent, StringRef(nmd.modifier.name)},
      [&]() { return &this->for_any_uncached<ModifierComputeContext>(parent, nmd); });
}

const ModifierComputeContext &ComputeContextCache::for_modifier(const ComputeContext *parent,
                                                                StringRef modifier_name)
{
  return *modifier_contexts_cache_.lookup_or_add_cb(std::pair{parent, modifier_name}, [&]() {
    return &this->for_any_uncached<ModifierComputeContext>(parent, modifier_name);
  });
}

const GroupNodeComputeContext &ComputeContextCache::for_group_node(const ComputeContext *parent,
                                                                   const int32_t node_id)
{
  return *group_node_contexts_cache_.lookup_or_add_cb(std::pair{parent, node_id}, [&]() {
    return &this->for_any_uncached<GroupNodeComputeContext>(parent, node_id);
  });
}

const GroupNodeComputeContext &ComputeContextCache::for_group_node(const ComputeContext *parent,
                                                                   const bNode &caller_group_node,
                                                                   const bNodeTree &caller_tree)
{
  return *group_node_contexts_cache_.lookup_or_add_cb(
      std::pair{parent, caller_group_node.identifier}, [&]() {
        return &this->for_any_uncached<GroupNodeComputeContext>(
            parent, caller_group_node, caller_tree);
      });
}

}  // namespace blender::bke
