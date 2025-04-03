/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_string_utf8.h"

#include "NOD_geo_closure.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

#include "BLO_read_write.hh"

namespace blender::nodes::node_geo_closure_cc {

/** Shared between closure input and output node. */
static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *current_node_ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(current_node_ptr->owner_id);
  bNode *current_node = static_cast<bNode *>(current_node_ptr->data);

  const bke::bNodeTreeZones *zones = ntree.zones();
  if (!zones) {
    return;
  }
  const bke::bNodeTreeZone *zone = zones->get_zone_by_node(current_node->identifier);
  if (!zone) {
    return;
  }
  if (!zone->output_node) {
    return;
  }
  bNode &output_node = const_cast<bNode &>(*zone->output_node);

  if (current_node->type_legacy == GEO_NODE_CLOSURE_INPUT) {
    if (uiLayout *panel = uiLayoutPanel(C, layout, "input_items", false, TIP_("Input Items"))) {
      socket_items::ui::draw_items_list_with_operators<ClosureInputItemsAccessor>(
          C, panel, ntree, output_node);
      socket_items::ui::draw_active_item_props<ClosureInputItemsAccessor>(
          ntree, output_node, [&](PointerRNA *item_ptr) {
            uiItemR(panel, item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
          });
    }
  }
  else {
    if (uiLayout *panel = uiLayoutPanel(C, layout, "output_items", false, TIP_("Output Items"))) {
      socket_items::ui::draw_items_list_with_operators<ClosureOutputItemsAccessor>(
          C, panel, ntree, output_node);
      socket_items::ui::draw_active_item_props<ClosureOutputItemsAccessor>(
          ntree, output_node, [&](PointerRNA *item_ptr) {
            uiItemR(panel, item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
          });
    }
  }
}

namespace input_node {

NODE_STORAGE_FUNCS(NodeGeometryClosureInput);

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();
  if (node && tree) {
    const NodeGeometryClosureInput &storage = node_storage(*node);
    const bNode *output_node = tree->node_by_id(storage.output_node_id);
    if (output_node) {
      const auto &output_storage = *static_cast<const NodeGeometryClosureOutput *>(
          output_node->storage);
      for (const int i : IndexRange(output_storage.input_items.items_num)) {
        const NodeGeometryClosureInputItem &item = output_storage.input_items.items[i];
        const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
        const std::string identifier = ClosureInputItemsAccessor::socket_identifier_for_item(item);
        b.add_output(socket_type, item.name, identifier);
      }
    }
  }
  b.add_output<decl::Extend>("", "__extend__");
}

static void node_label(const bNodeTree * /*ntree*/,
                       const bNode * /*node*/,
                       char *label,
                       const int label_maxncpy)
{
  BLI_strncpy_utf8(label, IFACE_("Closure"), label_maxncpy);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryClosureInput *data = MEM_callocN<NodeGeometryClosureInput>(__func__);
  node->storage = data;
}

static bool node_insert_link(bNodeTree *ntree, bNode *node, bNodeLink *link)
{
  bNode *output_node = ntree->node_by_id(node_storage(*node).output_node_id);
  if (!output_node) {
    return true;
  }
  return socket_items::try_add_item_via_any_extend_socket<ClosureInputItemsAccessor>(
      *ntree, *node, *output_node, *link);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeClosureInput", GEO_NODE_CLOSURE_INPUT);
  ntype.ui_name = "Closure Input";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = nullptr;
  ntype.initfunc = node_init;
  ntype.labelfunc = node_label;
  ntype.no_muting = true;
  ntype.insert_link = node_insert_link;
  ntype.draw_buttons_ex = node_layout_ex;
  blender::bke::node_type_storage(
      ntype, "NodeGeometryClosureInput", node_free_standard_storage, node_copy_standard_storage);
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace input_node

namespace output_node {

NODE_STORAGE_FUNCS(NodeGeometryClosureOutput);

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (node && tree) {
    const NodeGeometryClosureOutput &storage = node_storage(*node);
    for (const int i : IndexRange(storage.output_items.items_num)) {
      const NodeGeometryClosureOutputItem &item = storage.output_items.items[i];
      const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
      const std::string identifier = ClosureOutputItemsAccessor::socket_identifier_for_item(item);
      b.add_input(socket_type, item.name, identifier);
    }
  }
  b.add_input<decl::Extend>("", "__extend__");
  b.add_output<decl::Closure>("Closure");
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryClosureOutput *data = MEM_callocN<NodeGeometryClosureOutput>(__func__);
  node->storage = data;
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryClosureOutput &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_dupallocN<NodeGeometryClosureOutput>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<ClosureInputItemsAccessor>(*src_node, *dst_node);
  socket_items::copy_array<ClosureOutputItemsAccessor>(*src_node, *dst_node);
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<ClosureInputItemsAccessor>(*node);
  socket_items::destruct_array<ClosureOutputItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static bool node_insert_link(bNodeTree *ntree, bNode *node, bNodeLink *link)
{
  return socket_items::try_add_item_via_any_extend_socket<ClosureOutputItemsAccessor>(
      *ntree, *node, *node, *link);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ClosureInputItemsAccessor>();
  socket_items::ops::make_common_operators<ClosureOutputItemsAccessor>();
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeClosureOutput", GEO_NODE_CLOSURE_OUTPUT);
  ntype.ui_name = "Closure Output";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.labelfunc = input_node::node_label;
  ntype.no_muting = true;
  ntype.register_operators = node_operators;
  ntype.insert_link = node_insert_link;
  ntype.draw_buttons_ex = node_layout_ex;
  bke::node_type_storage(ntype, "NodeGeometryClosureOutput", node_free_storage, node_copy_storage);
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace output_node

}  // namespace blender::nodes::node_geo_closure_cc

namespace blender::nodes {

StructRNA *ClosureInputItemsAccessor::item_srna = &RNA_NodeGeometryClosureInputItem;
int ClosureInputItemsAccessor::node_type = GEO_NODE_CLOSURE_OUTPUT;
int ClosureInputItemsAccessor::item_dna_type = SDNA_TYPE_FROM_STRUCT(NodeGeometryClosureInputItem);

void ClosureInputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void ClosureInputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

StructRNA *ClosureOutputItemsAccessor::item_srna = &RNA_NodeGeometryClosureOutputItem;
int ClosureOutputItemsAccessor::node_type = GEO_NODE_CLOSURE_OUTPUT;
int ClosureOutputItemsAccessor::item_dna_type = SDNA_TYPE_FROM_STRUCT(
    NodeGeometryClosureOutputItem);

void ClosureOutputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void ClosureOutputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
