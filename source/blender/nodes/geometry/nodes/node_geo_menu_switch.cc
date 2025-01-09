/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "DNA_node_types.h"

#include "FN_multi_function.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "NOD_geo_menu_switch.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"

#include "BLO_read_write.hh"

#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "BKE_screen.hh"

#include "WM_api.hh"

namespace blender::nodes::node_geo_menu_switch_cc {

NODE_STORAGE_FUNCS(NodeMenuSwitch)

static bool is_supported_socket_type(const eNodeSocketDatatype data_type)
{
  return ELEM(data_type,
              SOCK_FLOAT,
              SOCK_INT,
              SOCK_BOOLEAN,
              SOCK_ROTATION,
              SOCK_VECTOR,
              SOCK_STRING,
              SOCK_RGBA,
              SOCK_GEOMETRY,
              SOCK_OBJECT,
              SOCK_COLLECTION,
              SOCK_MATERIAL,
              SOCK_IMAGE,
              SOCK_MATRIX);
}

static void node_declare(blender::nodes::NodeDeclarationBuilder &b)
{
  const bNodeTree *ntree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (node == nullptr) {
    return;
  }
  const NodeMenuSwitch &storage = node_storage(*node);
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(storage.data_type);
  const bool supports_fields = socket_type_supports_fields(data_type);

  auto &menu = b.add_input<decl::Menu>("Menu");
  if (supports_fields) {
    menu.supports_field();
  }

  for (const NodeEnumItem &enum_item : storage.enum_definition.items()) {
    const std::string identifier = MenuSwitchItemsAccessor::socket_identifier_for_item(enum_item);
    auto &input = b.add_input(data_type, enum_item.name, std::move(identifier))
                      .socket_name_ptr(
                          &ntree->id, MenuSwitchItemsAccessor::item_srna, &enum_item, "name");
    ;
    if (supports_fields) {
      input.supports_field();
    }
    /* Labels are ugly in combination with data-block pickers and are usually disabled. */
    input.hide_label(ELEM(data_type, SOCK_OBJECT, SOCK_IMAGE, SOCK_COLLECTION, SOCK_MATERIAL));
  }

  auto &output = b.add_output(data_type, "Output");
  if (supports_fields) {
    output.dependent_field().reference_pass_all();
  }
  else if (data_type == SOCK_GEOMETRY) {
    output.propagate_all();
  }

  b.add_input<decl::Extend>("", "__extend__");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeMenuSwitch *data = MEM_cnew<NodeMenuSwitch>(__func__);
  data->data_type = SOCK_GEOMETRY;
  data->enum_definition.next_identifier = 0;
  data->enum_definition.items_array = nullptr;
  data->enum_definition.items_num = 0;
  node->storage = data;

  socket_items::add_item_with_name<MenuSwitchItemsAccessor>(*node, "A");
  socket_items::add_item_with_name<MenuSwitchItemsAccessor>(*node, "B");
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<MenuSwitchItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeMenuSwitch &src_storage = node_storage(*src_node);
  NodeMenuSwitch *dst_storage = MEM_cnew<NodeMenuSwitch>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<MenuSwitchItemsAccessor>(*src_node, *dst_node);
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(params.other_socket().type);
  if (params.in_out() == SOCK_IN) {
    if (data_type == SOCK_MENU) {
      params.add_item(IFACE_("Menu"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeMenuSwitch");
        params.update_and_connect_available_socket(node, "Menu");
      });
    }
  }
  else {
    if (data_type != SOCK_MENU) {
      params.add_item(IFACE_("Output"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeMenuSwitch");
        node_storage(node).data_type = params.socket.type;
        params.update_and_connect_available_socket(node, "Output");
      });
    }
  }
}

/**
 * Multi-function which evaluates the switch input for each enum item and partially fills the
 * output array with values from the input array where the identifier matches.
 */
class MenuSwitchFn : public mf::MultiFunction {
  const NodeEnumDefinition &enum_def_;
  const CPPType &type_;
  mf::Signature signature_;

 public:
  MenuSwitchFn(const NodeEnumDefinition &enum_def, const CPPType &type)
      : enum_def_(enum_def), type_(type)
  {
    mf::SignatureBuilder builder{"Menu Switch", signature_};
    builder.single_input<int>("Menu");
    for (const NodeEnumItem &enum_item : enum_def.items()) {
      builder.single_input(enum_item.name, type);
    }
    builder.single_output("Output", type);

    this->set_signature(&signature_);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const
  {
    const int value_inputs_start = 1;
    const int inputs_num = enum_def_.items_num;
    const VArray<int> values = params.readonly_single_input<int>(0, "Menu");
    /* Use one extra mask at the end for invalid indices. */
    const int invalid_index = inputs_num;

    GMutableSpan output = params.uninitialized_single_output(
        signature_.params.index_range().last(), "Output");

    auto find_item_index = [&](const int value) -> int {
      for (const int i : enum_def_.items().index_range()) {
        const NodeEnumItem &item = enum_def_.items()[i];
        if (item.identifier == value) {
          return i;
        }
      }
      return invalid_index;
    };

    if (const std::optional<int> value = values.get_if_single()) {
      const int index = find_item_index(*value);
      if (index < inputs_num) {
        const GVArray inputs = params.readonly_single_input(value_inputs_start + index);
        inputs.materialize_to_uninitialized(mask, output.data());
      }
      else {
        type_.fill_construct_indices(type_.default_value(), output.data(), mask);
      }
      return;
    }

    IndexMaskMemory memory;
    Array<IndexMask> masks(inputs_num + 1);
    IndexMask::from_groups<int64_t>(
        mask, memory, [&](const int64_t i) { return find_item_index(values[i]); }, masks);

    for (const int i : IndexRange(inputs_num)) {
      if (!masks[i].is_empty()) {
        const GVArray inputs = params.readonly_single_input(value_inputs_start + i);
        inputs.materialize_to_uninitialized(masks[i], output.data());
      }
    }

    type_.fill_construct_indices(type_.default_value(), output.data(), masks[invalid_index]);
  }
};

class LazyFunctionForMenuSwitchNode : public LazyFunction {
 private:
  const bNode &node_;
  bool can_be_field_ = false;
  const NodeEnumDefinition &enum_def_;
  const CPPType *cpp_type_;
  const CPPType *field_base_type_;

 public:
  LazyFunctionForMenuSwitchNode(const bNode &node,
                                GeometryNodesLazyFunctionGraphInfo &lf_graph_info)
      : node_(node), enum_def_(node_storage(node).enum_definition)
  {
    const NodeMenuSwitch &storage = node_storage(node);
    const eNodeSocketDatatype data_type = eNodeSocketDatatype(storage.data_type);
    can_be_field_ = socket_type_supports_fields(data_type);
    const bke::bNodeSocketType *socket_type = bke::node_socket_type_find(
        *bke::node_static_socket_type(data_type, PROP_NONE));
    BLI_assert(socket_type != nullptr);
    cpp_type_ = socket_type->geometry_nodes_cpp_type;
    field_base_type_ = socket_type->base_cpp_type;

    MutableSpan<int> lf_index_by_bsocket = lf_graph_info.mapping.lf_index_by_bsocket;
    debug_name_ = node.name;
    lf_index_by_bsocket[node.input_socket(0).index_in_tree()] = inputs_.append_and_get_index_as(
        "Switch", CPPType::get<SocketValueVariant>(), lf::ValueUsage::Used);
    for (const int i : enum_def_.items().index_range()) {
      const NodeEnumItem &enum_item = enum_def_.items()[i];
      lf_index_by_bsocket[node.input_socket(i + 1).index_in_tree()] =
          inputs_.append_and_get_index_as(enum_item.name, *cpp_type_, lf::ValueUsage::Maybe);
    }
    lf_index_by_bsocket[node.output_socket(0).index_in_tree()] = outputs_.append_and_get_index_as(
        "Value", *cpp_type_);
  }

  void execute_impl(lf::Params &params, const lf::Context & /*context*/) const override
  {
    SocketValueVariant condition_variant = params.get_input<SocketValueVariant>(0);
    if (condition_variant.is_context_dependent_field() && can_be_field_) {
      this->execute_field(condition_variant.get<Field<int>>(), params);
    }
    else {
      this->execute_single(condition_variant.get<int>(), params);
    }
  }

  void execute_single(const int condition, lf::Params &params) const
  {
    for (const int i : IndexRange(enum_def_.items_num)) {
      const NodeEnumItem &enum_item = enum_def_.items_array[i];
      const int input_index = i + 1;
      if (enum_item.identifier == condition) {
        void *value_to_forward = params.try_get_input_data_ptr_or_request(input_index);
        if (value_to_forward == nullptr) {
          /* Try again when the value is available. */
          return;
        }

        void *output_ptr = params.get_output_data_ptr(0);
        cpp_type_->move_construct(value_to_forward, output_ptr);
        params.output_set(0);
      }
      else {
        params.set_input_unused(input_index);
      }
    }
    /* No guarantee that the switch input matches any enum,
     * set default outputs to ensure valid state. */
    set_default_remaining_node_outputs(params, node_);
  }

  void execute_field(Field<int> condition, lf::Params &params) const
  {
    /* When the condition is a non-constant field, we need all inputs. */
    const int values_num = this->enum_def_.items_num;
    Array<SocketValueVariant *, 8> input_values(values_num);
    for (const int i : IndexRange(values_num)) {
      const int input_index = i + 1;
      input_values[i] = params.try_get_input_data_ptr_or_request<SocketValueVariant>(input_index);
    }
    if (input_values.as_span().contains(nullptr)) {
      /* Try again when inputs are available. */
      return;
    }

    Vector<GField> item_fields(enum_def_.items_num + 1);
    item_fields[0] = std::move(condition);
    for (const int i : IndexRange(enum_def_.items_num)) {
      item_fields[i + 1] = input_values[i]->extract<GField>();
    }
    std::unique_ptr<MultiFunction> multi_function = std::make_unique<MenuSwitchFn>(
        enum_def_, *field_base_type_);
    GField output_field{FieldOperation::Create(std::move(multi_function), std::move(item_fields))};

    void *output_ptr = params.get_output_data_ptr(0);
    new (output_ptr) SocketValueVariant(std::move(output_field));
    params.output_set(0);
  }
};

/**
 * Outputs booleans that indicate which inputs of a menu switch node
 * are used. Note that it's possible that multiple inputs are used
 * when the condition is a field.
 */
class LazyFunctionForMenuSwitchSocketUsage : public lf::LazyFunction {
  const NodeEnumDefinition &enum_def_;

 public:
  LazyFunctionForMenuSwitchSocketUsage(const bNode &node)
      : enum_def_(node_storage(node).enum_definition)
  {
    debug_name_ = "Menu Switch Socket Usage";
    inputs_.append_as("Condition", CPPType::get<SocketValueVariant>());
    for (const int i : IndexRange(enum_def_.items_num)) {
      const NodeEnumItem &enum_item = enum_def_.items()[i];
      outputs_.append_as(enum_item.name, CPPType::get<bool>());
    }
  }

  void execute_impl(lf::Params &params, const lf::Context & /*context*/) const override
  {
    const SocketValueVariant &condition_variant = params.get_input<SocketValueVariant>(0);
    if (condition_variant.is_context_dependent_field()) {
      for (const int i : IndexRange(enum_def_.items_num)) {
        params.set_output(i, true);
      }
    }
    else {
      const int32_t value = condition_variant.get<int>();
      for (const int i : IndexRange(enum_def_.items_num)) {
        const NodeEnumItem &enum_item = enum_def_.items()[i];
        params.set_output(i, value == enum_item.identifier);
      }
    }
  }
};

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *static_cast<bNode *>(ptr->data);

  uiItemR(layout, ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);

  if (uiLayout *panel = uiLayoutPanel(C, layout, "menu_switch_items", false, TIP_("Menu Items"))) {
    socket_items::ui::draw_items_list_with_operators<MenuSwitchItemsAccessor>(
        C, panel, tree, node);
    socket_items::ui::draw_active_item_props<MenuSwitchItemsAccessor>(
        tree, node, [&](PointerRNA *item_ptr) {
          uiLayoutSetPropSep(panel, true);
          uiLayoutSetPropDecorate(panel, false);
          uiItemR(panel, item_ptr, "description", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static void node_operators()
{
  socket_items::ops::make_common_operators<MenuSwitchItemsAccessor>();
}

static bool node_insert_link(bNodeTree *ntree, bNode *node, bNodeLink *link)
{
  return socket_items::try_add_item_via_any_extend_socket<MenuSwitchItemsAccessor>(
      *ntree, *node, *node, *link);
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "data_type",
      "Data Type",
      "",
      rna_enum_node_socket_data_type_items,
      NOD_storage_enum_accessors(data_type),
      SOCK_GEOMETRY,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(
            rna_enum_node_socket_data_type_items, [](const EnumPropertyItem &item) -> bool {
              return is_supported_socket_type(eNodeSocketDatatype(item.value));
            });
      });
}

static void register_node()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeMenuSwitch", GEO_NODE_MENU_SWITCH, NODE_CLASS_CONVERTER);
  ntype.ui_name = "Menu Switch";
  ntype.ui_description = "Select from multiple inputs by name";
  ntype.enum_name_legacy = "MENU_SWITCH";
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(&ntype, "NodeMenuSwitch", node_free_storage, node_copy_storage);
  ntype.gather_link_search_ops = node_gather_link_searches;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  ntype.insert_link = node_insert_link;
  blender::bke::node_register_type(&ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(register_node)

}  // namespace blender::nodes::node_geo_menu_switch_cc

namespace blender::nodes {

std::unique_ptr<LazyFunction> get_menu_switch_node_lazy_function(
    const bNode &node, GeometryNodesLazyFunctionGraphInfo &lf_graph_info)
{
  using namespace node_geo_menu_switch_cc;
  BLI_assert(node.type_legacy == GEO_NODE_MENU_SWITCH);
  return std::make_unique<LazyFunctionForMenuSwitchNode>(node, lf_graph_info);
}

std::unique_ptr<LazyFunction> get_menu_switch_node_socket_usage_lazy_function(const bNode &node)
{
  using namespace node_geo_menu_switch_cc;
  BLI_assert(node.type_legacy == GEO_NODE_MENU_SWITCH);
  return std::make_unique<LazyFunctionForMenuSwitchSocketUsage>(node);
}

StructRNA *MenuSwitchItemsAccessor::item_srna = &RNA_NodeEnumItem;
int MenuSwitchItemsAccessor::node_type = GEO_NODE_MENU_SWITCH;
int MenuSwitchItemsAccessor::item_dna_type = SDNA_TYPE_FROM_STRUCT(NodeEnumItem);

void MenuSwitchItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
  BLO_write_string(writer, item.description);
}

void MenuSwitchItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
  BLO_read_string(reader, &item.description);
}

}  // namespace blender::nodes
