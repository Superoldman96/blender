/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "NOD_socket_items.hh"

namespace blender::nodes {

/**
 * Makes it possible to use various functions (e.g. the ones in `NOD_socket_items.hh`) with
 * simulation items.
 */
struct SimulationItemsAccessor {
  using ItemT = NodeSimulationItem;
  static StructRNA *item_srna;
  static int node_type;
  static int item_dna_type;
  static constexpr const char *node_idname = "GeometryNodeSimulationOutput";
  static constexpr bool has_type = true;
  static constexpr bool has_name = true;
  static constexpr bool has_single_identifier_str = true;
  struct operator_idnames {
    static constexpr const char *add_item = "NODE_OT_simulation_zone_item_add";
    static constexpr const char *remove_item = "NODE_OT_simulation_zone_item_remove";
    static constexpr const char *move_item = "NODE_OT_simulation_zone_item_move";
  };
  struct ui_idnames {
    static constexpr const char *list = "DATA_UL_simulation_zone_state";
  };
  struct rna_names {
    static constexpr const char *items = "state_items";
    static constexpr const char *active_index = "active_index";
  };

  static socket_items::SocketItemsRef<NodeSimulationItem> get_items_from_node(bNode &node)
  {
    auto *storage = static_cast<NodeGeometrySimulationOutput *>(node.storage);
    return {&storage->items, &storage->items_num, &storage->active_index};
  }

  static void copy_item(const NodeSimulationItem &src, NodeSimulationItem &dst)
  {
    dst = src;
    dst.name = BLI_strdup_null(dst.name);
  }

  static void destruct_item(NodeSimulationItem *item)
  {
    MEM_SAFE_FREE(item->name);
  }

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);

  static eNodeSocketDatatype get_socket_type(const NodeSimulationItem &item)
  {
    return eNodeSocketDatatype(item.socket_type);
  }

  static char **get_name(NodeSimulationItem &item)
  {
    return &item.name;
  }

  static bool supports_socket_type(const eNodeSocketDatatype socket_type)
  {
    /* Data-block types and closures are not supported. */
    return ELEM(socket_type,
                SOCK_FLOAT,
                SOCK_VECTOR,
                SOCK_RGBA,
                SOCK_BOOLEAN,
                SOCK_ROTATION,
                SOCK_MATRIX,
                SOCK_INT,
                SOCK_STRING,
                SOCK_GEOMETRY,
                SOCK_BUNDLE);
  }

  static void init_with_socket_type_and_name(bNode &node,
                                             NodeSimulationItem &item,
                                             const eNodeSocketDatatype socket_type,
                                             const char *name)
  {
    auto *storage = static_cast<NodeGeometrySimulationOutput *>(node.storage);
    item.socket_type = socket_type;
    item.identifier = storage->next_identifier++;
    socket_items::set_item_name_and_make_unique<SimulationItemsAccessor>(node, item, name);
  }

  static std::string socket_identifier_for_item(const NodeSimulationItem &item)
  {
    return "Item_" + std::to_string(item.identifier);
  }
};

}  // namespace blender::nodes
