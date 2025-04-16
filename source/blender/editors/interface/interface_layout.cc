/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "BLI_array.hh"
#include "BLI_dynstr.h"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_screen.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"

#include "ED_id_management.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"

using blender::StringRef;
using blender::StringRefNull;

/* Show an icon button after each RNA button to use to quickly set keyframes,
 * this is a way to display animation/driven/override status, see #54951. */
#define UI_PROP_DECORATE
/* Alternate draw mode where some buttons can use single icon width,
 * giving more room for the text at the expense of nicely aligned text. */
#define UI_PROP_SEP_ICON_WIDTH_EXCEPTION

/* -------------------------------------------------------------------- */
/** \name Structs and Defines
 * \{ */

#define UI_OPERATOR_ERROR_RET(_ot, _opname, return_statement) \
  if (ot == nullptr) { \
    ui_item_disabled(layout, _opname); \
    RNA_warning("'%s' unknown operator", _opname); \
    return_statement; \
  } \
  (void)0

#define UI_ITEM_PROP_SEP_DIVIDE 0.4f

/* uiLayoutRoot */

struct uiLayoutRoot {
  uiLayoutRoot *next, *prev;

  int type;
  wmOperatorCallContext opcontext;

  int emw, emh;
  int padding;

  uiMenuHandleFunc handlefunc;
  void *argv;

  const uiStyle *style;
  uiBlock *block;
  uiLayout *layout;
};

namespace blender::ui {
/* Item */

enum class ItemType : int8_t {
  Button,

  LayoutRow,
  LayoutPanelHeader,
  LayoutPanelBody,
  LayoutColumn,
  LayoutColumnFlow,
  LayoutRowFlow,
  LayoutGridFlow,
  LayoutBox,
  LayoutAbsolute,
  LayoutSplit,
  LayoutOverlap,
  LayoutRadial,

  LayoutRoot,
#if 0
  TemplateColumnFlow,
  TemplateSplit,
  TemplateBox,

  TemplateHeader,
  TemplateHeaderID,
#endif
};

enum class ItemInternalFlag : uint8_t {
  AutoFixedSize = 1 << 0,
  FixedSize = 1 << 1,

  BoxItem = 1 << 2, /* The item is "inside" a box item */
  PropSep = 1 << 3,
  InsidePropSep = 1 << 4,
  /* Show an icon button next to each property (to set keyframes, show status).
   * Enabled by default, depends on 'ItemInternalFlag::PropSep'. */
  PropDecorate = 1 << 5,
  PropDecorateNoPad = 1 << 6,
};
ENUM_OPERATORS(ItemInternalFlag, ItemInternalFlag::PropDecorateNoPad)

}  // namespace blender::ui

struct uiItem {
  blender::ui::ItemType type;
  blender::ui::ItemInternalFlag flag;

  uiItem() = default;
  uiItem(const uiItem &) = default;
  virtual ~uiItem() = default;
};

using uiItemType = blender::ui::ItemType;
using uiItemInternalFlag = blender::ui::ItemInternalFlag;

struct uiButtonItem : uiItem {
  uiBut *but;
};

struct uiLayout : uiItem {
  uiLayoutRoot *root;
  bContextStore *context;
  uiLayout *parent;
  blender::Vector<uiItem *> items;

  char heading[UI_MAX_NAME_STR];

  /** Sub layout to add child items, if not the layout itself. */
  uiLayout *child_items_layout;

  int x, y, w, h;
  float scale[2];
  short space;
  bool align;
  bool active;
  bool active_default;
  bool activate_init;
  bool enabled;
  bool redalert;
  bool keepaspect;
  /** For layouts inside grid-flow, they and their items shall never have a fixed maximal size. */
  bool variable_size;
  char alignment;
  blender::ui::EmbossType emboss;
  /** for fixed width or height to avoid UI size changes */
  float units[2];
  /** Is copied to uiButs created in this layout. */
  float search_weight;

  LayoutSuppressFlag suppress_flag;
};

struct uiLayoutItemFlow : uiLayout {
  int number;
  int totcol;
};

struct uiLayoutItemGridFlow : uiLayout {
  /* Extra parameters */
  bool row_major;    /* Fill first row first, instead of filling first column first. */
  bool even_columns; /* Same width for all columns. */
  bool even_rows;    /* Same height for all rows. */
  /**
   * - If positive, absolute fixed number of columns.
   * - If 0, fully automatic (based on available width).
   * - If negative, automatic but only generates number of columns/rows
   *   multiple of given (absolute) value.
   */
  int columns_len;

  /* Pure internal runtime storage. */
  int tot_items, tot_columns, tot_rows;
};

struct uiLayoutItemBx : uiLayout {
  uiBut *roundbox;
};

struct uiLayoutItemPanelHeader : uiLayout {
  PointerRNA open_prop_owner;
  char open_prop_name[64];
};

struct uiLayoutItemPanelBody : uiLayout {};

struct uiLayoutItemSplit : uiLayout {
  float percentage;
};

struct uiLayoutItemRoot : uiLayout {};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Item
 * \{ */

static StringRef ui_item_name_add_colon(StringRef name, char namestr[UI_MAX_NAME_STR])
{
  const int len = name.size();

  if (len != 0 && len + 1 < UI_MAX_NAME_STR) {
    memcpy(namestr, name.data(), len);
    namestr[len] = ':';
    namestr[len + 1] = '\0';
    return namestr;
  }

  return name;
}

static StringRefNull ui_item_name_add_colon(StringRefNull name, char namestr[UI_MAX_NAME_STR])
{
  const int len = name.size();

  if (len != 0 && len + 1 < UI_MAX_NAME_STR) {
    memcpy(namestr, name.data(), len);
    namestr[len] = ':';
    namestr[len + 1] = '\0';
    return namestr;
  }

  return name;
}

static int ui_item_fit(const int item,
                       const int pos,
                       const int all,
                       const int available,
                       const bool is_last,
                       const int alignment,
                       float *extra_pixel)
{
  /* available == 0 is unlimited */
  if (ELEM(0, available, all)) {
    return item;
  }

  if (all > available) {
    /* contents is bigger than available space */
    if (is_last) {
      return available - pos;
    }

    const float width = *extra_pixel + (item * available) / float(all);
    *extra_pixel = width - int(width);
    return int(width);
  }

  /* contents is smaller or equal to available space */
  if (alignment == UI_LAYOUT_ALIGN_EXPAND) {
    if (is_last) {
      return available - pos;
    }

    const float width = *extra_pixel + (item * available) / float(all);
    *extra_pixel = width - int(width);
    return int(width);
  }
  return item;
}

/* variable button size in which direction? */
#define UI_ITEM_VARY_X 1
#define UI_ITEM_VARY_Y 2

static int ui_layout_vary_direction(uiLayout *layout)
{
  return ((ELEM(layout->root->type, UI_LAYOUT_HEADER, UI_LAYOUT_PIEMENU) ||
           (layout->alignment != UI_LAYOUT_ALIGN_EXPAND)) ?
              UI_ITEM_VARY_X :
              UI_ITEM_VARY_Y);
}

static bool ui_layout_variable_size(uiLayout *layout)
{
  /* Note that this code is probably a bit flaky, we'd probably want to know whether it's
   * variable in X and/or Y, etc. But for now it mimics previous one,
   * with addition of variable flag set for children of grid-flow layouts. */
  return ui_layout_vary_direction(layout) == UI_ITEM_VARY_X || layout->variable_size;
}

/**
 * Factors to apply to #UI_UNIT_X when calculating button width.
 * This is used when the layout is a varying size, see #ui_layout_variable_size.
 */
struct uiTextIconPadFactor {
  float text;
  float icon;
  float icon_only;
};

/**
 * This adds over an icons width of padding even when no icon is used,
 * this is done because most buttons need additional space (drop-down chevron for example).
 * menus and labels use much smaller `text` values compared to this default.
 *
 * \note It may seem odd that the icon only adds 0.25, but taking margins into account it's fine,
 * except for #ui_text_pad_compact where a bit more margin is required.
 */
constexpr uiTextIconPadFactor ui_text_pad_default = {1.50f, 0.25f, 0.0f};

/** #ui_text_pad_default scaled down. */
constexpr uiTextIconPadFactor ui_text_pad_compact = {1.25f, 0.35f, 0.0f};

/** Least amount of padding not to clip the text or icon. */
constexpr uiTextIconPadFactor ui_text_pad_none = {0.25f, 1.50f, 0.0f};

/**
 * Estimated size of text + icon.
 */
static int ui_text_icon_width_ex(uiLayout *layout,
                                 const StringRef name,
                                 int icon,
                                 const uiTextIconPadFactor &pad_factor,
                                 const uiFontStyle *fstyle)
{
  const int unit_x = UI_UNIT_X * (layout->scale[0] ? layout->scale[0] : 1.0f);

  /* When there is no text, always behave as if this is an icon-only button
   * since it's not useful to return empty space. */
  if (icon && name.is_empty()) {
    return unit_x * (1.0f + pad_factor.icon_only);
  }

  if (ui_layout_variable_size(layout)) {
    if (!icon && name.is_empty()) {
      return unit_x * (1.0f + pad_factor.icon_only);
    }

    if (layout->alignment != UI_LAYOUT_ALIGN_EXPAND) {
      layout->flag |= uiItemInternalFlag::FixedSize;
    }

    float margin = pad_factor.text;
    if (icon) {
      margin += pad_factor.icon;
    }

    const float aspect = layout->root->block->aspect;
    return UI_fontstyle_string_width_with_block_aspect(fstyle, name, aspect) +
           int(ceilf(unit_x * margin));
  }
  return unit_x * 10;
}

static int ui_text_icon_width(uiLayout *layout,
                              const StringRef name,
                              const int icon,
                              const bool compact)
{
  return ui_text_icon_width_ex(
      layout, name, icon, compact ? ui_text_pad_compact : ui_text_pad_default, UI_FSTYLE_WIDGET);
}

static void ui_item_size(const uiItem *item, int *r_w, int *r_h)
{
  if (item->type == uiItemType::Button) {
    const uiButtonItem *bitem = static_cast<const uiButtonItem *>(item);

    if (r_w) {
      *r_w = BLI_rctf_size_x(&bitem->but->rect);
    }
    if (r_h) {
      *r_h = BLI_rctf_size_y(&bitem->but->rect);
    }
  }
  else {
    const uiLayout *litem = static_cast<const uiLayout *>(item);

    if (r_w) {
      *r_w = litem->w;
    }
    if (r_h) {
      *r_h = litem->h;
    }
  }
}

static void ui_item_offset(const uiItem *item, int *r_x, int *r_y)
{
  if (item->type == uiItemType::Button) {
    const uiButtonItem *bitem = static_cast<const uiButtonItem *>(item);

    if (r_x) {
      *r_x = bitem->but->rect.xmin;
    }
    if (r_y) {
      *r_y = bitem->but->rect.ymin;
    }
  }
  else {
    if (r_x) {
      *r_x = 0;
    }
    if (r_y) {
      *r_y = 0;
    }
  }
}

static void ui_item_position(uiItem *item, const int x, const int y, const int w, const int h)
{
  if (item->type == uiItemType::Button) {
    uiButtonItem *bitem = static_cast<uiButtonItem *>(item);

    bitem->but->rect.xmin = x;
    bitem->but->rect.ymin = y;
    bitem->but->rect.xmax = x + w;
    bitem->but->rect.ymax = y + h;

    ui_but_update(bitem->but); /* For `strlen`. */
  }
  else {
    uiLayout *litem = static_cast<uiLayout *>(item);

    litem->x = x;
    litem->y = y + h;
    litem->w = w;
    litem->h = h;
  }
}

static void ui_item_move(uiItem *item, const int delta_xmin, const int delta_xmax)
{
  if (item->type == uiItemType::Button) {
    uiButtonItem *bitem = static_cast<uiButtonItem *>(item);

    bitem->but->rect.xmin += delta_xmin;
    bitem->but->rect.xmax += delta_xmax;

    ui_but_update(bitem->but); /* For `strlen`. */
  }
  else {
    uiLayout *litem = static_cast<uiLayout *>(item);

    if (delta_xmin > 0) {
      litem->x += delta_xmin;
    }
    else {
      litem->w += delta_xmax;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Special RNA Items
 * \{ */

int uiLayoutGetLocalDir(const uiLayout *layout)
{
  switch (layout->type) {
    case uiItemType::LayoutRow:
    case uiItemType::LayoutRoot:
    case uiItemType::LayoutOverlap:
    case uiItemType::LayoutPanelHeader:
      return UI_LAYOUT_HORIZONTAL;
    case uiItemType::LayoutColumn:
    case uiItemType::LayoutColumnFlow:
    case uiItemType::LayoutGridFlow:
    case uiItemType::LayoutSplit:
    case uiItemType::LayoutAbsolute:
    case uiItemType::LayoutBox:
    case uiItemType::LayoutPanelBody:
    default:
      return UI_LAYOUT_VERTICAL;
  }
}

static uiLayout *ui_item_local_sublayout(uiLayout *test, uiLayout *layout, bool align)
{
  uiLayout *sub;
  if (uiLayoutGetLocalDir(test) == UI_LAYOUT_HORIZONTAL) {
    sub = uiLayoutRow(layout, align);
  }
  else {
    sub = uiLayoutColumn(layout, align);
  }

  sub->space = 0;
  return sub;
}

static void ui_layer_but_cb(bContext *C, void *arg_but, void *arg_index)
{
  wmWindow *win = CTX_wm_window(C);
  uiBut *but = static_cast<uiBut *>(arg_but);
  PointerRNA *ptr = &but->rnapoin;
  PropertyRNA *prop = but->rnaprop;
  const int index = POINTER_AS_INT(arg_index);
  const bool shift = win->eventstate->modifier & KM_SHIFT;
  const int len = RNA_property_array_length(ptr, prop);

  if (!shift) {
    BLI_assert(index < len);
    blender::Array<bool, RNA_STACK_ARRAY> value_array(len);
    value_array.fill(false);
    value_array[index] = true;

    RNA_property_boolean_set_array(ptr, prop, value_array.data());

    RNA_property_update(C, ptr, prop);

    for (const std::unique_ptr<uiBut> &cbut : but->block->buttons) {
      ui_but_update(cbut.get());
    }
  }
}

/* create buttons for an item with an RNA array */
static void ui_item_array(uiLayout *layout,
                          uiBlock *block,
                          const StringRefNull name,
                          int icon,
                          PointerRNA *ptr,
                          PropertyRNA *prop,
                          const int len,
                          int x,
                          const int y,
                          int w,
                          const int /*h*/,
                          const bool expand,
                          const bool slider,
                          const int toggle,
                          const bool icon_only,
                          const bool compact,
                          const bool show_text)
{
  const uiStyle *style = layout->root->style;

  /* retrieve type and subtype */
  const PropertyType type = RNA_property_type(prop);
  const PropertySubType subtype = RNA_property_subtype(prop);

  uiLayout *sub = ui_item_local_sublayout(layout, layout, true);
  UI_block_layout_set_current(block, sub);

  /* create label */
  if (!name.is_empty() && show_text) {
    uiDefBut(block, UI_BTYPE_LABEL, 0, name, 0, 0, w, UI_UNIT_Y, nullptr, 0.0, 0.0, "");
  }

  /* create buttons */
  if (type == PROP_BOOLEAN && ELEM(subtype, PROP_LAYER, PROP_LAYER_MEMBER)) {
    /* special check for layer layout */
    const int cols = (len >= 20) ? 2 : 1;
    const int colbuts = len / (2 * cols);
    uint layer_used = 0;
    uint layer_active = 0;

    UI_block_layout_set_current(block, uiLayoutAbsolute(layout, false));

    const int butw = UI_UNIT_X * 0.75;
    const int buth = UI_UNIT_X * 0.75;

    for (int b = 0; b < cols; b++) {
      UI_block_align_begin(block);

      for (int a = 0; a < colbuts; a++) {
        const int layer_num = a + b * colbuts;
        const uint layer_flag = (1u << layer_num);

        if (layer_used & layer_flag) {
          if (layer_active & layer_flag) {
            icon = ICON_LAYER_ACTIVE;
          }
          else {
            icon = ICON_LAYER_USED;
          }
        }
        else {
          icon = ICON_BLANK1;
        }

        uiBut *but = uiDefAutoButR(
            block, ptr, prop, layer_num, "", icon, x + butw * a, y + buth, butw, buth);
        if (subtype == PROP_LAYER_MEMBER) {
          UI_but_func_set(but, ui_layer_but_cb, but, POINTER_FROM_INT(layer_num));
        }
      }
      for (int a = 0; a < colbuts; a++) {
        const int layer_num = a + len / 2 + b * colbuts;
        const uint layer_flag = (1u << layer_num);

        if (layer_used & layer_flag) {
          if (layer_active & layer_flag) {
            icon = ICON_LAYER_ACTIVE;
          }
          else {
            icon = ICON_LAYER_USED;
          }
        }
        else {
          icon = ICON_BLANK1;
        }

        uiBut *but = uiDefAutoButR(
            block, ptr, prop, layer_num, "", icon, x + butw * a, y, butw, buth);
        if (subtype == PROP_LAYER_MEMBER) {
          UI_but_func_set(but, ui_layer_but_cb, but, POINTER_FROM_INT(layer_num));
        }
      }
      UI_block_align_end(block);

      x += colbuts * butw + style->buttonspacex;
    }
  }
  else if (subtype == PROP_MATRIX) {
    int totdim, dim_size[3]; /* 3 == RNA_MAX_ARRAY_DIMENSION */
    int row, col;

    UI_block_layout_set_current(block, uiLayoutAbsolute(layout, true));

    totdim = RNA_property_array_dimension(ptr, prop, dim_size);
    if (totdim != 2) {
      /* Only 2D matrices supported in UI so far. */
      return;
    }

    w /= dim_size[0];
    // h /= dim_size[1]; /* UNUSED */

    for (int a = 0; a < len; a++) {
      col = a % dim_size[0];
      row = a / dim_size[0];

      uiBut *but = uiDefAutoButR(block,
                                 ptr,
                                 prop,
                                 a,
                                 "",
                                 ICON_NONE,
                                 x + w * col,
                                 y + (dim_size[1] * UI_UNIT_Y) - (row * UI_UNIT_Y),
                                 w,
                                 UI_UNIT_Y);
      if (slider && but->type == UI_BTYPE_NUM) {
        uiButNumber *number_but = (uiButNumber *)but;
        const float step_size = number_but->step_size;
        const float precision = number_but->precision;
        but = ui_but_change_type(but, UI_BTYPE_NUM_SLIDER);
        uiButNumberSlider *slider_but = reinterpret_cast<uiButNumberSlider *>(but);
        slider_but->step_size = step_size;
        slider_but->precision = precision;
      }
    }
  }
  else if (subtype == PROP_DIRECTION && !expand) {
    uiDefButR_prop(block,
                   UI_BTYPE_UNITVEC,
                   0,
                   name,
                   x,
                   y,
                   UI_UNIT_X * 3,
                   UI_UNIT_Y * 3,
                   ptr,
                   prop,
                   -1,
                   0,
                   0,
                   std::nullopt);
  }
  else {
    /* NOTE: this block of code is a bit arbitrary and has just been made
     * to work with common cases, but may need to be re-worked */

    /* special case, boolean array in a menu, this could be used in a more generic way too */
    if (ELEM(subtype, PROP_COLOR, PROP_COLOR_GAMMA) && !expand && ELEM(len, 3, 4)) {
      uiDefAutoButR(block, ptr, prop, -1, "", ICON_NONE, 0, 0, w, UI_UNIT_Y);
    }
    else {
      /* Even if 'expand' is false, we expand anyway. */

      /* Layout for known array sub-types. */
      char str[3] = {'\0'};

      if (!icon_only && show_text) {
        if (type != PROP_BOOLEAN) {
          str[1] = ':';
        }
      }

      /* Show check-boxes for rna on a non-emboss block (menu for eg). */
      bool *boolarr = nullptr;
      if (type == PROP_BOOLEAN && ELEM(layout->root->block->emboss,
                                       blender::ui::EmbossType::None,
                                       blender::ui::EmbossType::Pulldown))
      {
        boolarr = static_cast<bool *>(MEM_callocN(sizeof(bool) * len, __func__));
        RNA_property_boolean_get_array(ptr, prop, boolarr);
      }

      const char *str_buf = show_text ? str : "";
      for (int a = 0; a < len; a++) {
        if (!icon_only && show_text) {
          str[0] = RNA_property_array_item_char(prop, a);
        }
        if (boolarr) {
          icon = boolarr[a] ? ICON_CHECKBOX_HLT : ICON_CHECKBOX_DEHLT;
        }

        const int width_item = ((compact && type == PROP_BOOLEAN) ?
                                    min_ii(w, ui_text_icon_width(layout, str_buf, icon, false)) :
                                    w);

        uiBut *but = uiDefAutoButR(
            block, ptr, prop, a, str_buf, icon, 0, 0, width_item, UI_UNIT_Y);
        if (slider && but->type == UI_BTYPE_NUM) {
          uiButNumber *number_but = (uiButNumber *)but;
          const float step_size = number_but->step_size;
          const float precision = number_but->precision;
          but = ui_but_change_type(but, UI_BTYPE_NUM_SLIDER);
          uiButNumberSlider *slider_but = reinterpret_cast<uiButNumberSlider *>(but);
          slider_but->step_size = step_size;
          slider_but->precision = precision;
        }
        if ((toggle == 1) && but->type == UI_BTYPE_CHECKBOX) {
          but->type = UI_BTYPE_TOGGLE;
        }
        if ((a == 0) && (subtype == PROP_AXISANGLE)) {
          UI_but_unit_type_set(but, PROP_UNIT_ROTATION);
        }
      }

      if (boolarr) {
        MEM_freeN(boolarr);
      }
    }
  }

  UI_block_layout_set_current(block, layout);
}

static void ui_item_enum_expand_handle(bContext *C, void *arg1, void *arg2)
{
  wmWindow *win = CTX_wm_window(C);

  if ((win->eventstate->modifier & KM_SHIFT) == 0) {
    uiBut *but = (uiBut *)arg1;
    const int enum_value = POINTER_AS_INT(arg2);

    int current_value = RNA_property_enum_get(&but->rnapoin, but->rnaprop);
    if (!(current_value & enum_value)) {
      current_value = enum_value;
    }
    else {
      current_value &= enum_value;
    }
    RNA_property_enum_set(&but->rnapoin, but->rnaprop, current_value);
  }
}

/**
 * Draw a single enum button, a utility for #ui_item_enum_expand_exec
 */
static void ui_item_enum_expand_elem_exec(uiLayout *layout,
                                          uiBlock *block,
                                          PointerRNA *ptr,
                                          PropertyRNA *prop,
                                          const std::optional<StringRef> uiname,
                                          const int h,
                                          const eButType but_type,
                                          const bool icon_only,
                                          const EnumPropertyItem *item,
                                          const bool is_first)
{
  const char *name = (!uiname || !uiname->is_empty()) ? item->name : "";
  const int icon = item->icon;
  const int value = item->value;
  const int itemw = ui_text_icon_width(block->curlayout, icon_only ? "" : name, icon, false);

  uiBut *but;
  if (icon && name[0] && !icon_only) {
    but = uiDefIconTextButR_prop(
        block, but_type, 0, icon, name, 0, 0, itemw, h, ptr, prop, -1, 0, value, std::nullopt);
  }
  else if (icon) {
    const int w = (is_first) ? itemw : ceilf(itemw - U.pixelsize);
    but = uiDefIconButR_prop(
        block, but_type, 0, icon, 0, 0, w, h, ptr, prop, -1, 0, value, std::nullopt);
  }
  else {
    but = uiDefButR_prop(
        block, but_type, 0, name, 0, 0, itemw, h, ptr, prop, -1, 0, value, std::nullopt);
  }

  if (RNA_property_flag(prop) & PROP_ENUM_FLAG) {
    /* If this is set, assert since we're clobbering someone else's callback. */
    /* Buttons get their block's func by default, so we cannot assert in that case either. */
    BLI_assert(ELEM(but->func, nullptr, block->func));
    UI_but_func_set(but, ui_item_enum_expand_handle, but, POINTER_FROM_INT(value));
  }

  if (uiLayoutGetLocalDir(layout) != UI_LAYOUT_HORIZONTAL) {
    but->drawflag |= UI_BUT_TEXT_LEFT;
  }

  /* Allow quick, inaccurate swipe motions to switch tabs
   * (no need to keep cursor over them). */
  if (but_type == UI_BTYPE_TAB) {
    but->flag |= UI_BUT_DRAG_LOCK;
  }
}

static void ui_item_enum_expand_exec(uiLayout *layout,
                                     uiBlock *block,
                                     PointerRNA *ptr,
                                     PropertyRNA *prop,
                                     const std::optional<StringRef> uiname,
                                     const int h,
                                     const eButType but_type,
                                     const bool icon_only)
{
  /* XXX: The way this function currently handles uiname parameter
   * is insane and inconsistent with general UI API:
   *
   * - uiname is the *enum property* label.
   * - when it is nullptr or empty, we do not draw *enum items* labels,
   *   this doubles the icon_only parameter.
   * - we *never* draw (i.e. really use) the enum label uiname, it is just used as a mere flag!
   *
   * Unfortunately, fixing this implies an API "soft break", so better to defer it for later... :/
   * - mont29
   */

  BLI_assert(RNA_property_type(prop) == PROP_ENUM);

  const bool radial = (layout->root->type == UI_LAYOUT_PIEMENU);

  bool free;
  const EnumPropertyItem *item_array;
  if (radial) {
    RNA_property_enum_items_gettexted_all(
        static_cast<bContext *>(block->evil_C), ptr, prop, &item_array, nullptr, &free);
  }
  else {
    RNA_property_enum_items_gettexted(
        static_cast<bContext *>(block->evil_C), ptr, prop, &item_array, nullptr, &free);
  }

  /* We don't want nested rows, cols in menus. */
  uiLayout *layout_radial = nullptr;
  if (radial) {
    if (layout->root->layout == layout) {
      layout_radial = uiLayoutRadial(layout);
      UI_block_layout_set_current(block, layout_radial);
    }
    else {
      if (layout->type == uiItemType::LayoutRadial) {
        layout_radial = layout;
      }
      UI_block_layout_set_current(block, layout);
    }
  }
  else if (ELEM(layout->type, uiItemType::LayoutGridFlow, uiItemType::LayoutColumnFlow) ||
           layout->root->type == UI_LAYOUT_MENU)
  {
    UI_block_layout_set_current(block, layout);
  }
  else {
    UI_block_layout_set_current(block, ui_item_local_sublayout(layout, layout, true));
  }

  for (const EnumPropertyItem *item = item_array; item->identifier; item++) {
    const bool is_first = item == item_array;

    if (!item->identifier[0]) {
      const EnumPropertyItem *next_item = item + 1;

      /* Separate items, potentially with a label. */
      if (next_item->identifier) {
        /* Item without identifier but with name:
         * Add group label for the following items. */
        if (item->name) {
          if (!is_first) {
            uiItemS(block->curlayout);
          }
          uiItemL(block->curlayout, item->name, item->icon);
        }
        else if (radial && layout_radial) {
          uiItemS(layout_radial);
        }
        else {
          uiItemS(block->curlayout);
        }
      }
      continue;
    }

    ui_item_enum_expand_elem_exec(
        layout, block, ptr, prop, uiname, h, but_type, icon_only, item, is_first);
  }

  UI_block_layout_set_current(block, layout);

  if (free) {
    MEM_freeN((void *)item_array);
  }
}
static void ui_item_enum_expand(uiLayout *layout,
                                uiBlock *block,
                                PointerRNA *ptr,
                                PropertyRNA *prop,
                                const std::optional<StringRef> uiname,
                                const int h,
                                const bool icon_only)
{
  ui_item_enum_expand_exec(layout, block, ptr, prop, uiname, h, UI_BTYPE_ROW, icon_only);
}
static void ui_item_enum_expand_tabs(uiLayout *layout,
                                     bContext *C,
                                     uiBlock *block,
                                     PointerRNA *ptr,
                                     PropertyRNA *prop,
                                     PointerRNA *ptr_highlight,
                                     PropertyRNA *prop_highlight,
                                     const std::optional<StringRef> uiname,
                                     const int h,
                                     const bool icon_only)
{
  const int start_size = block->buttons.size();

  ui_item_enum_expand_exec(layout, block, ptr, prop, uiname, h, UI_BTYPE_TAB, icon_only);

  if (block->buttons.is_empty()) {
    return;
  }

  BLI_assert(start_size != block->buttons.size());

  for (int i = start_size; i < block->buttons.size(); i++) {
    uiBut *tab = block->buttons[i].get();
    UI_but_drawflag_enable(tab, ui_but_align_opposite_to_area_align_get(CTX_wm_region(C)));
    if (icon_only) {
      UI_but_drawflag_enable(tab, UI_BUT_HAS_TOOLTIP_LABEL);
    }
  }

  const bool use_custom_highlight = (prop_highlight != nullptr);

  if (use_custom_highlight) {
    const int highlight_array_len = RNA_property_array_length(ptr_highlight, prop_highlight);
    blender::Array<bool, 64> highlight_array(highlight_array_len);
    RNA_property_boolean_get_array(ptr_highlight, prop_highlight, highlight_array.data());
    const int end = std::min<int>(start_size + highlight_array_len, block->buttons.size());
    for (int i = start_size; i < end; i++) {
      uiBut *tab_but = block->buttons[i].get();
      SET_FLAG_FROM_TEST(tab_but->flag, !highlight_array[i], UI_BUT_INACTIVE);
    }
  }
}

/* callback for keymap item change button */
static void ui_keymap_but_cb(bContext * /*C*/, void *but_v, void * /*key_v*/)
{
  uiBut *but = static_cast<uiBut *>(but_v);
  BLI_assert(but->type == UI_BTYPE_HOTKEY_EVENT);
  const uiButHotkeyEvent *hotkey_but = (uiButHotkeyEvent *)but;

  RNA_int_set(
      &but->rnapoin, "shift", (hotkey_but->modifier_key & KM_SHIFT) ? KM_MOD_HELD : KM_NOTHING);
  RNA_int_set(
      &but->rnapoin, "ctrl", (hotkey_but->modifier_key & KM_CTRL) ? KM_MOD_HELD : KM_NOTHING);
  RNA_int_set(
      &but->rnapoin, "alt", (hotkey_but->modifier_key & KM_ALT) ? KM_MOD_HELD : KM_NOTHING);
  RNA_int_set(
      &but->rnapoin, "oskey", (hotkey_but->modifier_key & KM_OSKEY) ? KM_MOD_HELD : KM_NOTHING);
  RNA_int_set(
      &but->rnapoin, "hyper", (hotkey_but->modifier_key & KM_HYPER) ? KM_MOD_HELD : KM_NOTHING);
}

/**
 * Create label + button for RNA property
 *
 * \param w_hint: For varying width layout, this becomes the label width.
 *                Otherwise it's used to fit both items into it.
 */
static uiBut *ui_item_with_label(uiLayout *layout,
                                 uiBlock *block,
                                 const StringRefNull name,
                                 const int icon,
                                 PointerRNA *ptr,
                                 PropertyRNA *prop,
                                 const int index,
                                 const int x,
                                 const int y,
                                 const int w_hint,
                                 const int h,
                                 const int flag)
{
  uiLayout *sub = layout;
  int prop_but_width = w_hint;
#ifdef UI_PROP_DECORATE
  uiLayout *layout_prop_decorate = nullptr;
  const bool use_prop_sep = bool(layout->flag & uiItemInternalFlag::PropSep);
  const bool use_prop_decorate = use_prop_sep &&
                                 bool(layout->flag & uiItemInternalFlag::PropDecorate) &&
                                 !bool(layout->flag & uiItemInternalFlag::PropDecorateNoPad);
#endif

  const bool is_keymapitem_ptr = RNA_struct_is_a(ptr->type, &RNA_KeyMapItem);
  if ((flag & UI_ITEM_R_FULL_EVENT) && !is_keymapitem_ptr) {
    RNA_warning("Data is not a keymap item struct: %s. Ignoring 'full_event' option.",
                RNA_struct_identifier(ptr->type));
  }

  UI_block_layout_set_current(block, layout);

  /* Only add new row if more than 1 item will be added. */
  if (!name.is_empty()
#ifdef UI_PROP_DECORATE
      || use_prop_decorate
#endif
  )
  {
    /* Also avoid setting 'align' if possible. Set the space to zero instead as aligning a large
     * number of labels can end up aligning thousands of buttons when displaying key-map search (a
     * heavy operation), see: #78636. */
    sub = uiLayoutRow(layout, layout->align);
    sub->space = 0;
  }

  if (!name.is_empty()) {
#ifdef UI_PROP_DECORATE
    if (use_prop_sep) {
      layout_prop_decorate = uiItemL_respect_property_split(layout, name, ICON_NONE);
    }
    else
#endif
    {
      int w_label;
      if (ui_layout_variable_size(layout)) {
        /* In this case, a pure label without additional padding.
         * Use a default width for property button(s). */
        prop_but_width = UI_UNIT_X * 5;
        w_label = ui_text_icon_width_ex(
            layout, name, ICON_NONE, ui_text_pad_none, UI_FSTYLE_WIDGET);
      }
      else {
        w_label = w_hint / 3;
      }
      uiDefBut(block, UI_BTYPE_LABEL, 0, name, x, y, w_label, h, nullptr, 0.0, 0.0, "");
    }
  }

  const PropertyType type = RNA_property_type(prop);
  const PropertySubType subtype = RNA_property_subtype(prop);

  uiBut *but;
  if (ELEM(subtype, PROP_FILEPATH, PROP_DIRPATH)) {
    UI_block_layout_set_current(block, uiLayoutRow(sub, true));
    but = uiDefAutoButR(block, ptr, prop, index, "", icon, x, y, prop_but_width - UI_UNIT_X, h);

    if (but != nullptr) {
      if (ELEM(subtype, PROP_FILEPATH, PROP_DIRPATH)) {
        if ((RNA_property_flag(prop) & PROP_PATH_SUPPORTS_BLEND_RELATIVE) == 0) {
          if (BLI_path_is_rel(but->drawstr.c_str())) {

            /* Finally check that this isn't suppressed, see: #137507. */
            if ((uiLayoutSuppressFlagGet(layout) &
                 LayoutSuppressFlag::PathSupportsBlendFileRelative) != LayoutSuppressFlag(0))
            {
              UI_but_flag_enable(but, UI_BUT_REDALERT);
            }
          }
        }
      }
    }

    /* #BUTTONS_OT_file_browse calls #UI_context_active_but_prop_get_filebrowser. */
    uiDefIconButO(block,
                  UI_BTYPE_BUT,
                  subtype == PROP_DIRPATH ? "BUTTONS_OT_directory_browse" :
                                            "BUTTONS_OT_file_browse",
                  WM_OP_INVOKE_DEFAULT,
                  ICON_FILEBROWSER,
                  x,
                  y,
                  UI_UNIT_X,
                  h,
                  std::nullopt);
  }
  else if (flag & UI_ITEM_R_EVENT) {
    but = uiDefButR_prop(block,
                         UI_BTYPE_KEY_EVENT,
                         0,
                         name,
                         x,
                         y,
                         prop_but_width,
                         h,
                         ptr,
                         prop,
                         index,
                         0,
                         0,
                         std::nullopt);
  }
  else if ((flag & UI_ITEM_R_FULL_EVENT) && is_keymapitem_ptr) {
    std::string kmi_str =
        WM_keymap_item_to_string(static_cast<const wmKeyMapItem *>(ptr->data), false).value_or("");

    but = uiDefButR_prop(block,
                         UI_BTYPE_HOTKEY_EVENT,
                         0,
                         kmi_str,
                         x,
                         y,
                         prop_but_width,
                         h,
                         ptr,
                         prop,
                         0,
                         0,
                         0,
                         std::nullopt);
    UI_but_func_set(but, ui_keymap_but_cb, but, nullptr);
  }
  else {
    const std::optional<StringRefNull> str = (type == PROP_ENUM && !(flag & UI_ITEM_R_ICON_ONLY)) ?
                                                 std::nullopt :
                                                 std::make_optional<StringRefNull>("");
    but = uiDefAutoButR(block, ptr, prop, index, str, icon, x, y, prop_but_width, h);
  }

  if (flag & UI_ITEM_R_IMMEDIATE) {
    UI_but_flag_enable(but, UI_BUT_ACTIVATE_ON_INIT);
  }

#ifdef UI_PROP_DECORATE
  /* Only for alignment. */
  if (use_prop_decorate) { /* Note that sep flag may have been unset meanwhile. */
    uiItemL(layout_prop_decorate ? layout_prop_decorate : sub, nullptr, ICON_BLANK1);
  }
#endif /* UI_PROP_DECORATE */

  UI_block_layout_set_current(block, layout);
  return but;
}

void UI_context_active_but_prop_get_filebrowser(const bContext *C,
                                                PointerRNA *r_ptr,
                                                PropertyRNA **r_prop,
                                                bool *r_is_undo,
                                                bool *r_is_userdef)
{
  ARegion *region = CTX_wm_region_popup(C) ? CTX_wm_region_popup(C) : CTX_wm_region(C);
  uiBut *prevbut = nullptr;

  *r_ptr = {};
  *r_prop = nullptr;
  *r_is_undo = false;
  *r_is_userdef = false;

  if (!region) {
    return;
  }

  LISTBASE_FOREACH (uiBlock *, block, &region->runtime->uiblocks) {
    for (const std::unique_ptr<uiBut> &but : block->buttons) {
      if (but && but->rnapoin.data) {
        if (RNA_property_type(but->rnaprop) == PROP_STRING) {
          prevbut = but.get();
        }
      }

      /* find the button before the active one */
      if ((but->flag & UI_BUT_LAST_ACTIVE) && prevbut) {
        *r_ptr = prevbut->rnapoin;
        *r_prop = prevbut->rnaprop;
        *r_is_undo = (prevbut->flag & UI_BUT_UNDO) != 0;
        *r_is_userdef = UI_but_is_userdef(prevbut);
        return;
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Button Items
 * \{ */

/**
 * Update a buttons tip with an enum's description if possible.
 */
static void ui_but_tip_from_enum_item(uiBut *but, const EnumPropertyItem *item)
{
  if (but->tip == nullptr || but->tip[0] == '\0') {
    if (item->description && item->description[0] &&
        !(but->optype && but->optype->get_description))
    {
      but->tip = item->description;
    }
  }
}

/* disabled item */
static void ui_item_disabled(uiLayout *layout, const char *name)
{
  uiBlock *block = layout->root->block;

  UI_block_layout_set_current(block, layout);

  if (!name) {
    name = "";
  }

  const int w = ui_text_icon_width(layout, name, 0, false);

  uiBut *but = uiDefBut(block, UI_BTYPE_LABEL, 0, name, 0, 0, w, UI_UNIT_Y, nullptr, 0.0, 0.0, "");
  UI_but_disable(but, "");
}

/**
 * Operator Item
 * \param r_opptr: Optional, initialize with operator properties when not nullptr.
 * Will always be written to even in the case of errors.
 */
static uiBut *uiItemFullO_ptr_ex(uiLayout *layout,
                                 wmOperatorType *ot,
                                 std::optional<StringRef> name,
                                 int icon,
                                 IDProperty *properties,
                                 const wmOperatorCallContext context,
                                 const eUI_Item_Flag flag,
                                 PointerRNA *r_opptr)
{
  /* Take care to fill 'r_opptr' whatever happens. */
  uiBlock *block = layout->root->block;

  std::string operator_name;
  if (!name) {
    if (ot && ot->srna && (flag & UI_ITEM_R_ICON_ONLY) == 0) {
      operator_name = WM_operatortype_name(ot, nullptr);
      name = operator_name.c_str();
    }
    else {
      name = "";
    }
  }

  if (layout->root->type == UI_LAYOUT_MENU && !icon) {
    icon = ICON_BLANK1;
  }

  UI_block_layout_set_current(block, layout);
  ui_block_new_button_group(block, uiButtonGroupFlag(0));

  const int w = ui_text_icon_width(layout, *name, icon, false);

  const blender::ui::EmbossType prev_emboss = layout->emboss;
  if (flag & UI_ITEM_R_NO_BG) {
    layout->emboss = blender::ui::EmbossType::NoneOrStatus;
  }

  /* create the button */
  uiBut *but;
  if (icon) {
    if (!name->is_empty()) {
      but = uiDefIconTextButO_ptr(
          block, UI_BTYPE_BUT, ot, context, icon, *name, 0, 0, w, UI_UNIT_Y, std::nullopt);
    }
    else {
      but = uiDefIconButO_ptr(
          block, UI_BTYPE_BUT, ot, context, icon, 0, 0, w, UI_UNIT_Y, std::nullopt);
    }
  }
  else {
    but = uiDefButO_ptr(block, UI_BTYPE_BUT, ot, context, *name, 0, 0, w, UI_UNIT_Y, std::nullopt);
  }

  BLI_assert(but->optype != nullptr);

  if (flag & UI_ITEM_R_NO_BG) {
    layout->emboss = prev_emboss;
  }

  if (flag & UI_ITEM_O_DEPRESS) {
    but->flag |= UI_SELECT_DRAW;
  }

  if (flag & UI_ITEM_R_ICON_ONLY) {
    UI_but_drawflag_disable(but, UI_BUT_ICON_LEFT);
  }

  if (layout->redalert) {
    UI_but_flag_enable(but, UI_BUT_REDALERT);
  }

  if (layout->active_default) {
    UI_but_flag_enable(but, UI_BUT_ACTIVE_DEFAULT);
  }

  /* assign properties */
  if (properties || r_opptr) {
    PointerRNA *opptr = UI_but_operator_ptr_ensure(but);
    if (properties) {
      opptr->data = properties;
    }
    else {
      opptr->data = blender::bke::idprop::create_group("wmOperatorProperties").release();
    }
    if (r_opptr) {
      *r_opptr = *opptr;
    }
  }

  return but;
}

static void ui_item_menu_hold(bContext *C, ARegion *butregion, uiBut *but)
{
  uiPopupMenu *pup = UI_popup_menu_begin(C, "", ICON_NONE);
  uiLayout *layout = UI_popup_menu_layout(pup);
  uiBlock *block = layout->root->block;
  UI_popup_menu_but_set(pup, butregion, but);

  block->flag |= UI_BLOCK_POPUP_HOLD;

  char direction = UI_DIR_DOWN;
  if (but->drawstr.empty()) {
    switch (RGN_ALIGN_ENUM_FROM_MASK(butregion->alignment)) {
      case RGN_ALIGN_LEFT:
        direction = UI_DIR_RIGHT;
        break;
      case RGN_ALIGN_RIGHT:
        direction = UI_DIR_LEFT;
        break;
      case RGN_ALIGN_BOTTOM:
        direction = UI_DIR_UP;
        break;
      default:
        direction = UI_DIR_DOWN;
        break;
    }
  }
  UI_block_direction_set(block, direction);

  const char *menu_id = static_cast<const char *>(but->hold_argN);
  MenuType *mt = WM_menutype_find(menu_id, true);
  if (mt) {
    uiLayoutSetContextFromBut(layout, but);
    UI_menutype_draw(C, mt, layout);
  }
  else {
    uiItemL(layout, RPT_("Menu Missing:"), ICON_NONE);
    uiItemL(layout, menu_id, ICON_NONE);
  }
  UI_popup_menu_end(C, pup);
}

void uiItemFullO_ptr(uiLayout *layout,
                     wmOperatorType *ot,
                     std::optional<StringRef> name,
                     const int icon,
                     IDProperty *properties,
                     const wmOperatorCallContext context,
                     const eUI_Item_Flag flag,
                     PointerRNA *r_opptr)
{
  uiItemFullO_ptr_ex(layout, ot, name, icon, properties, context, flag, r_opptr);
}

void uiItemFullOMenuHold_ptr(uiLayout *layout,
                             wmOperatorType *ot,
                             std::optional<StringRef> name,
                             int icon,
                             IDProperty *properties,
                             const wmOperatorCallContext context,
                             const eUI_Item_Flag flag,
                             const char *menu_id,
                             PointerRNA *r_opptr)
{
  uiBut *but = uiItemFullO_ptr_ex(layout, ot, name, icon, properties, context, flag, r_opptr);
  UI_but_func_hold_set(but, ui_item_menu_hold, BLI_strdup(menu_id));
}

void uiItemFullO(uiLayout *layout,
                 const blender::StringRefNull opname,
                 const std::optional<StringRef> name,
                 int icon,
                 IDProperty *properties,
                 wmOperatorCallContext context,
                 const eUI_Item_Flag flag,
                 PointerRNA *r_opptr)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */

  UI_OPERATOR_ERROR_RET(ot, opname.c_str(), {
    if (r_opptr) {
      *r_opptr = PointerRNA_NULL;
    }
    return;
  });

  uiItemFullO_ptr(layout, ot, name, icon, properties, context, flag, r_opptr);
}

static StringRef ui_menu_enumpropname(uiLayout *layout,
                                      PointerRNA *ptr,
                                      PropertyRNA *prop,
                                      int retval)
{
  bool free;
  const EnumPropertyItem *item;
  RNA_property_enum_items(
      static_cast<bContext *>(layout->root->block->evil_C), ptr, prop, &item, nullptr, &free);

  const char *name;
  if (RNA_enum_name(item, retval, &name)) {
    name = CTX_IFACE_(RNA_property_translation_context(prop), name);
  }
  else {
    name = "";
  }

  if (free) {
    MEM_freeN((void *)item);
  }

  return name;
}

void uiItemEnumO_ptr(uiLayout *layout,
                     wmOperatorType *ot,
                     std::optional<StringRef> name,
                     int icon,
                     const StringRefNull propname,
                     int value)
{
  PointerRNA ptr;
  WM_operator_properties_create_ptr(&ptr, ot);

  PropertyRNA *prop = RNA_struct_find_property(&ptr, propname.c_str());
  if (prop == nullptr) {
    RNA_warning("%s.%s not found", RNA_struct_identifier(ptr.type), propname.c_str());
    return;
  }

  RNA_property_enum_set(&ptr, prop, value);

  name = name.value_or(ui_menu_enumpropname(layout, &ptr, prop, value));

  uiItemFullO_ptr(layout,
                  ot,
                  name,
                  icon,
                  static_cast<IDProperty *>(ptr.data),
                  layout->root->opcontext,
                  UI_ITEM_NONE,
                  nullptr);
}
void uiItemEnumO(uiLayout *layout,
                 const StringRefNull opname,
                 const std::optional<StringRef> name,
                 int icon,
                 const StringRefNull propname,
                 int value)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */

  if (ot) {
    uiItemEnumO_ptr(layout, ot, name, icon, propname, value);
  }
  else {
    ui_item_disabled(layout, opname.c_str());
    RNA_warning("unknown operator '%s'", opname.c_str());
  }
}

BLI_INLINE bool ui_layout_is_radial(const uiLayout *layout)
{
  return (layout->type == uiItemType::LayoutRadial) ||
         ((layout->type == uiItemType::LayoutRoot) && (layout->root->type == UI_LAYOUT_PIEMENU));
}

void uiItemsFullEnumO_items(uiLayout *layout,
                            wmOperatorType *ot,
                            const PointerRNA &ptr,
                            PropertyRNA *prop,
                            IDProperty *properties,
                            wmOperatorCallContext context,
                            eUI_Item_Flag flag,
                            const EnumPropertyItem *item_array,
                            int totitem,
                            int active)
{
  const StringRefNull propname = RNA_property_identifier(prop);
  if (RNA_property_type(prop) != PROP_ENUM) {
    RNA_warning("%s.%s, not an enum type", RNA_struct_identifier(ptr.type), propname.c_str());
    return;
  }

  uiLayout *target, *split = nullptr;
  uiBlock *block = layout->root->block;
  const bool radial = ui_layout_is_radial(layout);

  if (radial) {
    target = uiLayoutRadial(layout);
  }
  else if ((uiLayoutGetLocalDir(layout) == UI_LAYOUT_HORIZONTAL) && (flag & UI_ITEM_R_ICON_ONLY)) {
    target = layout;
    UI_block_layout_set_current(block, target);

    /* Add a blank button to the beginning of the row. */
    uiDefIconBut(block,
                 UI_BTYPE_LABEL,
                 0,
                 ICON_BLANK1,
                 0,
                 0,
                 1.25f * UI_UNIT_X,
                 UI_UNIT_Y,
                 nullptr,
                 0,
                 0,
                 std::nullopt);
  }
  else {
    split = uiLayoutSplit(layout, 0.0f, false);
    target = uiLayoutColumn(split, layout->align);
  }

  bool last_iter = false;
  const EnumPropertyItem *item = item_array;
  for (int i = 1; item->identifier && !last_iter; i++, item++) {
    /* Handle over-sized pies. */
    if (radial && (totitem > PIE_MAX_ITEMS) && (i >= PIE_MAX_ITEMS)) {
      if (item->name) { /* only visible items */
        const EnumPropertyItem *tmp;

        /* Check if there are more visible items for the next level. If not, we don't
         * add a new level and add the remaining item instead of the 'more' button. */
        for (tmp = item + 1; tmp->identifier; tmp++) {
          if (tmp->name) {
            break;
          }
        }

        if (tmp->identifier) { /* only true if loop above found item and did early-exit */
          ui_pie_menu_level_create(
              block, ot, propname, properties, item_array, totitem, context, flag);
          /* break since rest of items is handled in new pie level */
          break;
        }
        last_iter = true;
      }
      else {
        continue;
      }
    }

    if (item->identifier[0]) {
      PointerRNA tptr;
      WM_operator_properties_create_ptr(&tptr, ot);
      if (properties) {
        if (tptr.data) {
          IDP_FreeProperty(static_cast<IDProperty *>(tptr.data));
        }
        tptr.data = IDP_CopyProperty(properties);
      }
      RNA_property_enum_set(&tptr, prop, item->value);

      uiItemFullO_ptr(target,
                      ot,
                      (flag & UI_ITEM_R_ICON_ONLY) ? nullptr : item->name,
                      item->icon,
                      static_cast<IDProperty *>(tptr.data),
                      context,
                      flag,
                      nullptr);

      uiBut *but = block->buttons.last().get();

      if (active == (i - 1)) {
        but->flag |= UI_SELECT_DRAW;
      }

      ui_but_tip_from_enum_item(but, item);
    }
    else {
      if (item->name) {
        if (item != item_array && !radial && split != nullptr) {
          target = uiLayoutColumn(split, layout->align);
        }

        uiBut *but;
        if (item->icon || radial) {
          uiItemL(target, item->name, item->icon);

          but = block->buttons.last().get();
        }
        else {
          /* Do not use uiItemL here, as our root layout is a menu one,
           * it will add a fake blank icon! */
          but = uiDefBut(block,
                         UI_BTYPE_LABEL,
                         0,
                         item->name,
                         0,
                         0,
                         UI_UNIT_X * 5,
                         UI_UNIT_Y,
                         nullptr,
                         0.0,
                         0.0,
                         "");
          uiItemS(target);
        }
        ui_but_tip_from_enum_item(but, item);
      }
      else {
        if (radial) {
          /* invisible dummy button to ensure all items are
           * always at the same position */
          uiItemS(target);
        }
        else {
          /* XXX bug here, columns draw bottom item badly */
          uiItemS(target);
        }
      }
    }
  }
}

void uiItemsFullEnumO(uiLayout *layout,
                      const StringRefNull opname,
                      const StringRefNull propname,
                      IDProperty *properties,
                      wmOperatorCallContext context,
                      eUI_Item_Flag flag,
                      const int active)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */

  if (!ot || !ot->srna) {
    ui_item_disabled(layout, opname.c_str());
    RNA_warning("%s '%s'", ot ? "operator missing srna" : "unknown operator", opname.c_str());
    return;
  }

  PointerRNA ptr;
  WM_operator_properties_create_ptr(&ptr, ot);
  /* so the context is passed to itemf functions (some need it) */
  WM_operator_properties_sanitize(&ptr, false);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, propname.c_str());

  /* don't let bad properties slip through */
  BLI_assert((prop == nullptr) || (RNA_property_type(prop) == PROP_ENUM));

  uiBlock *block = layout->root->block;
  if (prop && RNA_property_type(prop) == PROP_ENUM) {
    const EnumPropertyItem *item_array = nullptr;
    int totitem;
    bool free;

    if (ui_layout_is_radial(layout)) {
      /* XXX: While "_all()" guarantees spatial stability,
       * it's bad when an enum has > 8 items total,
       * but only a small subset will ever be shown at once
       * (e.g. Mode Switch menu, after the introduction of GP editing modes).
       */
#if 0
      RNA_property_enum_items_gettexted_all(
          static_cast<bContext *>(block->evil_C), &ptr, prop, &item_array, &totitem, &free);
#else
      RNA_property_enum_items_gettexted(
          static_cast<bContext *>(block->evil_C), &ptr, prop, &item_array, &totitem, &free);
#endif
    }
    else {
      bContext *C = static_cast<bContext *>(block->evil_C);
      const bContextStore *previous_ctx = CTX_store_get(C);
      CTX_store_set(C, layout->context);
      RNA_property_enum_items_gettexted(C, &ptr, prop, &item_array, &totitem, &free);
      CTX_store_set(C, previous_ctx);
    }

    /* add items */
    uiItemsFullEnumO_items(
        layout, ot, ptr, prop, properties, context, flag, item_array, totitem, active);

    if (free) {
      MEM_freeN((void *)item_array);
    }
  }
  else if (prop && RNA_property_type(prop) != PROP_ENUM) {
    RNA_warning("%s.%s, not an enum type", RNA_struct_identifier(ptr.type), propname.c_str());
    return;
  }
  else {
    RNA_warning("%s.%s not found", RNA_struct_identifier(ptr.type), propname.c_str());
    return;
  }
}

void uiItemsEnumO(uiLayout *layout, const StringRefNull opname, const StringRefNull propname)
{
  uiItemsFullEnumO(layout, opname, propname, nullptr, layout->root->opcontext, UI_ITEM_NONE);
}

void uiItemEnumO_value(uiLayout *layout,
                       const StringRefNull name,
                       int icon,
                       const StringRefNull opname,
                       const StringRefNull propname,
                       int value)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */
  UI_OPERATOR_ERROR_RET(ot, opname.c_str(), return);

  PointerRNA ptr;
  WM_operator_properties_create_ptr(&ptr, ot);

  /* enum lookup */
  PropertyRNA *prop = RNA_struct_find_property(&ptr, propname.c_str());
  if (prop == nullptr) {
    RNA_warning("%s.%s not found", RNA_struct_identifier(ptr.type), propname.c_str());
    return;
  }

  RNA_property_enum_set(&ptr, prop, value);

  uiItemFullO_ptr(layout,
                  ot,
                  name,
                  icon,
                  static_cast<IDProperty *>(ptr.data),
                  layout->root->opcontext,
                  UI_ITEM_NONE,
                  nullptr);
}

void uiItemEnumO_string(uiLayout *layout,
                        const StringRef name,
                        int icon,
                        const StringRefNull opname,
                        const StringRefNull propname,
                        const char *value_str)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */
  UI_OPERATOR_ERROR_RET(ot, opname.c_str(), return);

  PointerRNA ptr;
  WM_operator_properties_create_ptr(&ptr, ot);

  PropertyRNA *prop = RNA_struct_find_property(&ptr, propname.c_str());
  if (prop == nullptr) {
    RNA_warning("%s.%s not found", RNA_struct_identifier(ptr.type), propname.c_str());
    return;
  }

  /* enum lookup */
  /* no need for translations here */
  const EnumPropertyItem *item;
  bool free;
  RNA_property_enum_items(
      static_cast<bContext *>(layout->root->block->evil_C), &ptr, prop, &item, nullptr, &free);

  int value;
  if (item == nullptr || RNA_enum_value_from_id(item, value_str, &value) == 0) {
    if (free) {
      MEM_freeN((void *)item);
    }
    RNA_warning(
        "%s.%s, enum %s not found", RNA_struct_identifier(ptr.type), propname.c_str(), value_str);
    return;
  }

  if (free) {
    MEM_freeN((void *)item);
  }

  RNA_property_enum_set(&ptr, prop, value);

  uiItemFullO_ptr(layout,
                  ot,
                  name,
                  icon,
                  static_cast<IDProperty *>(ptr.data),
                  layout->root->opcontext,
                  UI_ITEM_NONE,
                  nullptr);
}

void uiItemBooleanO(uiLayout *layout,
                    const std::optional<StringRef> name,
                    int icon,
                    const StringRefNull opname,
                    const StringRefNull propname,
                    int value)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */
  UI_OPERATOR_ERROR_RET(ot, opname.c_str(), return);

  PointerRNA ptr;
  WM_operator_properties_create_ptr(&ptr, ot);
  RNA_boolean_set(&ptr, propname.c_str(), value);

  uiItemFullO_ptr(layout,
                  ot,
                  name,
                  icon,
                  static_cast<IDProperty *>(ptr.data),
                  layout->root->opcontext,
                  UI_ITEM_NONE,
                  nullptr);
}

void uiItemIntO(uiLayout *layout,
                const std::optional<StringRef> name,
                int icon,
                const StringRefNull opname,
                const StringRefNull propname,
                int value)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */
  UI_OPERATOR_ERROR_RET(ot, opname.c_str(), return);

  PointerRNA ptr;
  WM_operator_properties_create_ptr(&ptr, ot);
  RNA_int_set(&ptr, propname.c_str(), value);

  uiItemFullO_ptr(layout,
                  ot,
                  name,
                  icon,
                  static_cast<IDProperty *>(ptr.data),
                  layout->root->opcontext,
                  UI_ITEM_NONE,
                  nullptr);
}

void uiItemFloatO(uiLayout *layout,
                  const std::optional<StringRef> name,
                  int icon,
                  const StringRefNull opname,
                  const StringRefNull propname,
                  float value)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */

  UI_OPERATOR_ERROR_RET(ot, opname.c_str(), return);

  PointerRNA ptr;
  WM_operator_properties_create_ptr(&ptr, ot);
  RNA_float_set(&ptr, propname.c_str(), value);

  uiItemFullO_ptr(layout,
                  ot,
                  name,
                  icon,
                  static_cast<IDProperty *>(ptr.data),
                  layout->root->opcontext,
                  UI_ITEM_NONE,
                  nullptr);
}

void uiItemStringO(uiLayout *layout,
                   const std::optional<StringRef> name,
                   int icon,
                   const StringRefNull opname,
                   const StringRefNull propname,
                   const char *value)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */

  UI_OPERATOR_ERROR_RET(ot, opname.c_str(), return);

  PointerRNA ptr;
  WM_operator_properties_create_ptr(&ptr, ot);
  RNA_string_set(&ptr, propname.c_str(), value);

  uiItemFullO_ptr(layout,
                  ot,
                  name,
                  icon,
                  static_cast<IDProperty *>(ptr.data),
                  layout->root->opcontext,
                  UI_ITEM_NONE,
                  nullptr);
}

void uiItemO(uiLayout *layout,
             const std::optional<StringRef> name,
             int icon,
             const StringRefNull opname)
{
  uiItemFullO(layout, opname, name, icon, nullptr, layout->root->opcontext, UI_ITEM_NONE, nullptr);
}

/* RNA property items */

static void ui_item_rna_size(uiLayout *layout,
                             StringRef name,
                             int icon,
                             PointerRNA *ptr,
                             PropertyRNA *prop,
                             int index,
                             bool icon_only,
                             bool compact,
                             int *r_w,
                             int *r_h)
{
  int w = 0, h;

  /* arbitrary extended width by type */
  const PropertyType type = RNA_property_type(prop);
  const PropertySubType subtype = RNA_property_subtype(prop);
  const int len = RNA_property_array_length(ptr, prop);

  bool is_checkbox_only = false;
  if (name.is_empty() && !icon_only) {
    if (ELEM(type, PROP_STRING, PROP_POINTER)) {
      name = "non-empty text";
    }
    else if (type == PROP_BOOLEAN) {
      if (icon == ICON_NONE) {
        /* Exception for check-boxes, they need a little less space to align nicely. */
        is_checkbox_only = true;
      }
      icon = ICON_DOT;
    }
    else if (type == PROP_ENUM) {
      /* Find the longest enum item name, instead of using a dummy text! */
      const EnumPropertyItem *item_array;
      bool free;
      RNA_property_enum_items_gettexted(static_cast<bContext *>(layout->root->block->evil_C),
                                        ptr,
                                        prop,
                                        &item_array,
                                        nullptr,
                                        &free);

      for (const EnumPropertyItem *item = item_array; item->identifier; item++) {
        if (item->identifier[0]) {
          w = max_ii(w, ui_text_icon_width(layout, item->name, item->icon, compact));
        }
      }
      if (free) {
        MEM_freeN((void *)item_array);
      }
    }
  }

  if (!w) {
    if (type == PROP_ENUM && icon_only) {
      w = ui_text_icon_width(layout, "", ICON_BLANK1, compact);
      if (index != RNA_ENUM_VALUE) {
        w += 0.6f * UI_UNIT_X;
      }
    }
    else {
      /* not compact for float/int buttons, looks too squashed */
      w = ui_text_icon_width(
          layout, name, icon, ELEM(type, PROP_FLOAT, PROP_INT) ? false : compact);
    }
  }
  h = UI_UNIT_Y;

  /* increase height for arrays */
  if (index == RNA_NO_INDEX && len > 0) {
    if (name.is_empty() && icon == ICON_NONE) {
      h = 0;
    }
    if (bool(layout->flag & uiItemInternalFlag::PropSep)) {
      h = 0;
    }
    if (ELEM(subtype, PROP_LAYER, PROP_LAYER_MEMBER)) {
      h += 2 * UI_UNIT_Y;
    }
    else if (subtype == PROP_MATRIX) {
      h += ceilf(sqrtf(len)) * UI_UNIT_Y;
    }
    else {
      h += len * UI_UNIT_Y;
    }
  }

  /* Increase width requirement if in a variable size layout. */
  if (ui_layout_variable_size(layout)) {
    if (type == PROP_BOOLEAN && !name.is_empty()) {
      w += UI_UNIT_X / 5;
    }
    else if (is_checkbox_only) {
      w -= UI_UNIT_X / 4;
    }
    else if (type == PROP_ENUM && !icon_only) {
      w += UI_UNIT_X / 4;
    }
    else if (ELEM(type, PROP_FLOAT, PROP_INT)) {
      w += UI_UNIT_X * 3;
    }
  }

  *r_w = w;
  *r_h = h;
}

static bool ui_item_rna_is_expand(PropertyRNA *prop, int index, const eUI_Item_Flag item_flag)
{
  const bool is_array = RNA_property_array_check(prop);
  const int subtype = RNA_property_subtype(prop);
  return is_array && (index == RNA_NO_INDEX) &&
         ((item_flag & UI_ITEM_R_EXPAND) ||
          !ELEM(subtype, PROP_COLOR, PROP_COLOR_GAMMA, PROP_DIRECTION));
}

/**
 * Find first layout ancestor (or self) with a heading set.
 *
 * \returns the layout to add the heading to as fallback (i.e. if it can't be placed in a split
 *          layout). Its #uiLayout.heading member can be cleared to mark the heading as added (so
 *          it's not added multiple times). Returns a pointer to the heading
 */
static uiLayout *ui_layout_heading_find(uiLayout *cur_layout)
{
  for (uiLayout *parent = cur_layout; parent; parent = parent->parent) {
    if (parent->heading[0]) {
      return parent;
    }
  }

  return nullptr;
}

static void ui_layout_heading_label_add(uiLayout *layout,
                                        uiLayout *heading_layout,
                                        bool right_align,
                                        bool respect_prop_split)
{
  const int prev_alignment = layout->alignment;

  if (right_align) {
    uiLayoutSetAlignment(layout, UI_LAYOUT_ALIGN_RIGHT);
  }

  if (respect_prop_split) {
    uiItemL_respect_property_split(layout, heading_layout->heading, ICON_NONE);
  }
  else {
    uiItemL(layout, heading_layout->heading, ICON_NONE);
  }
  /* After adding the heading label, we have to mark it somehow as added, so it's not added again
   * for other items in this layout. For now just clear it. */
  heading_layout->heading[0] = '\0';

  layout->alignment = prev_alignment;
}

/**
 * Hack to add further items in a row into the second part of the split layout, so the label part
 * keeps a fixed size.
 * \return The layout to place further items in for the split layout.
 */
static uiLayout *ui_item_prop_split_layout_hack(uiLayout *layout_parent, uiLayout *layout_split)
{
  /* Tag item as using property split layout, this is inherited to children so they can get special
   * treatment if needed. */
  layout_parent->flag |= uiItemInternalFlag::InsidePropSep;

  if (layout_parent->type == uiItemType::LayoutRow) {
    /* Prevent further splits within the row. */
    uiLayoutSetPropSep(layout_parent, false);

    layout_parent->child_items_layout = uiLayoutRow(layout_split, true);
    return layout_parent->child_items_layout;
  }
  return layout_split;
}

void uiItemFullR(uiLayout *layout,
                 PointerRNA *ptr,
                 PropertyRNA *prop,
                 int index,
                 int value,
                 eUI_Item_Flag flag,
                 const std::optional<StringRefNull> name_opt,
                 int icon,
                 const std::optional<StringRefNull> placeholder)
{
  uiBlock *block = layout->root->block;
  char namestr[UI_MAX_NAME_STR];
  const bool use_prop_sep = bool(layout->flag & uiItemInternalFlag::PropSep);
  const bool inside_prop_sep = bool(layout->flag & uiItemInternalFlag::InsidePropSep);
  /* Columns can define a heading to insert. If the first item added to a split layout doesn't have
   * a label to display in the first column, the heading is inserted there. Otherwise it's inserted
   * as a new row before the first item. */
  uiLayout *heading_layout = ui_layout_heading_find(layout);
  /* Although check-boxes use the split layout, they are an exception and should only place their
   * label in the second column, to not make that almost empty.
   *
   * Keep using 'use_prop_sep' instead of disabling it entirely because
   * we need the ability to have decorators still. */
  bool use_prop_sep_split_label = use_prop_sep;
  bool use_split_empty_name = (flag & UI_ITEM_R_SPLIT_EMPTY_NAME);

#ifdef UI_PROP_DECORATE
  struct DecorateInfo {
    bool use_prop_decorate;
    int len;
    uiLayout *layout;
    uiBut *but;
  };
  DecorateInfo ui_decorate{};
  ui_decorate.use_prop_decorate = (bool(layout->flag & uiItemInternalFlag::PropDecorate) &&
                                   use_prop_sep);

#endif /* UI_PROP_DECORATE */

  UI_block_layout_set_current(block, layout);
  ui_block_new_button_group(block, uiButtonGroupFlag(0));

  /* retrieve info */
  const PropertyType type = RNA_property_type(prop);
  const bool is_array = RNA_property_array_check(prop);
  const int len = (is_array) ? RNA_property_array_length(ptr, prop) : 0;
  const bool is_id_name_prop = (ptr->owner_id == ptr->data && type == PROP_STRING &&
                                prop == RNA_struct_name_property(ptr->type));

  const bool icon_only = (flag & UI_ITEM_R_ICON_ONLY) != 0;

  /* Boolean with -1 to signify that the value depends on the presence of an icon. */
  const int toggle = ((flag & UI_ITEM_R_TOGGLE) ? 1 : ((flag & UI_ITEM_R_ICON_NEVER) ? 0 : -1));
  const bool no_icon = (toggle == 0);

  /* set name and icon */
  StringRefNull name = name_opt.value_or(icon_only ? "" : RNA_property_ui_name(prop));

  if (type != PROP_BOOLEAN) {
    flag &= ~UI_ITEM_R_CHECKBOX_INVERT;
  }

  if (flag & UI_ITEM_R_ICON_ONLY) {
    /* pass */
  }
  else if (ELEM(type, PROP_INT, PROP_FLOAT, PROP_STRING, PROP_POINTER)) {
    if (use_prop_sep == false) {
      name = ui_item_name_add_colon(name, namestr);
    }
  }
  else if (type == PROP_BOOLEAN && is_array && index == RNA_NO_INDEX) {
    if (use_prop_sep == false) {
      name = ui_item_name_add_colon(name, namestr);
    }
  }
  else if (type == PROP_ENUM && index != RNA_ENUM_VALUE) {
    if (flag & UI_ITEM_R_COMPACT) {
      name = "";
    }
    else {
      if (use_prop_sep == false) {
        name = ui_item_name_add_colon(name, namestr);
      }
    }
  }

  if (no_icon == false) {
    if (icon == ICON_NONE) {
      icon = RNA_property_ui_icon(prop);
    }

    /* Menus and pie-menus don't show checkbox without this. */
    if ((layout->root->type == UI_LAYOUT_MENU) ||
        /* Use check-boxes only as a fallback in pie-menu's, when no icon is defined. */
        ((layout->root->type == UI_LAYOUT_PIEMENU) && (icon == ICON_NONE)))
    {
      const int prop_flag = RNA_property_flag(prop);
      if (type == PROP_BOOLEAN) {
        if ((is_array == false) || (index != RNA_NO_INDEX)) {
          if (prop_flag & PROP_ICONS_CONSECUTIVE) {
            icon = ICON_CHECKBOX_DEHLT; /* but->iconadd will set to correct icon */
          }
          else if (is_array) {
            icon = RNA_property_boolean_get_index(ptr, prop, index) ? ICON_CHECKBOX_HLT :
                                                                      ICON_CHECKBOX_DEHLT;
          }
          else {
            icon = RNA_property_boolean_get(ptr, prop) ? ICON_CHECKBOX_HLT : ICON_CHECKBOX_DEHLT;
          }
        }
      }
      else if (type == PROP_ENUM) {
        if (index == RNA_ENUM_VALUE) {
          const int enum_value = RNA_property_enum_get(ptr, prop);
          if (prop_flag & PROP_ICONS_CONSECUTIVE) {
            icon = ICON_CHECKBOX_DEHLT; /* but->iconadd will set to correct icon */
          }
          else if (prop_flag & PROP_ENUM_FLAG) {
            icon = (enum_value & value) ? ICON_CHECKBOX_HLT : ICON_CHECKBOX_DEHLT;
          }
          else {
            /* Only a single value can be chosen, so display as radio buttons. */
            icon = (enum_value == value) ? ICON_RADIOBUT_ON : ICON_RADIOBUT_OFF;
          }
        }
      }
    }
  }

#ifdef UI_PROP_SEP_ICON_WIDTH_EXCEPTION
  if (use_prop_sep) {
    if (type == PROP_BOOLEAN && (icon == ICON_NONE) && !icon_only) {
      use_prop_sep_split_label = false;
      /* For check-boxes we make an exception: We allow showing them in a split row even without
       * label. It typically relates to its neighbor items, so no need for an extra label. */
      use_split_empty_name = true;
    }
  }
#endif

  if ((type == PROP_ENUM) && (RNA_property_flag(prop) & PROP_ENUM_FLAG)) {
    flag |= UI_ITEM_R_EXPAND;
  }

  const bool slider = (flag & UI_ITEM_R_SLIDER) != 0;
  const bool expand = (flag & UI_ITEM_R_EXPAND) != 0;
  const bool no_bg = (flag & UI_ITEM_R_NO_BG) != 0;
  const bool compact = (flag & UI_ITEM_R_COMPACT) != 0;

  /* get size */
  int w, h;
  ui_item_rna_size(layout, name, icon, ptr, prop, index, icon_only, compact, &w, &h);

  const blender::ui::EmbossType prev_emboss = layout->emboss;
  if (no_bg) {
    layout->emboss = blender::ui::EmbossType::NoneOrStatus;
  }

  uiBut *but = nullptr;

  /* Split the label / property. */
  uiLayout *layout_parent = layout;

  if (use_prop_sep) {
    uiLayout *layout_row = nullptr;
#ifdef UI_PROP_DECORATE
    if (ui_decorate.use_prop_decorate) {
      layout_row = uiLayoutRow(layout, true);
      layout_row->space = 0;
      ui_decorate.len = max_ii(1, len);
    }
#endif /* UI_PROP_DECORATE */

    if (name.is_empty() && !use_split_empty_name) {
      /* Ensure we get a column when text is not set. */
      layout = uiLayoutColumn(layout_row ? layout_row : layout, true);
      layout->space = 0;
      if (heading_layout) {
        ui_layout_heading_label_add(layout, heading_layout, false, false);
      }
    }
    else {
      uiLayout *layout_split = uiLayoutSplit(
          layout_row ? layout_row : layout, UI_ITEM_PROP_SEP_DIVIDE, true);
      bool label_added = false;
      uiLayout *layout_sub = uiLayoutColumn(layout_split, true);
      layout_sub->space = 0;

      if (!use_prop_sep_split_label) {
        /* Pass */
      }
      else if (ui_item_rna_is_expand(prop, index, flag)) {
        char name_with_suffix[UI_MAX_DRAW_STR + 2];
        char str[2] = {'\0'};
        for (int a = 0; a < len; a++) {
          str[0] = RNA_property_array_item_char(prop, a);
          const bool use_prefix = (a == 0 && !name.is_empty());
          if (use_prefix) {
            char *s = name_with_suffix;
            s += STRNCPY_RLEN(name_with_suffix, name.c_str());
            *s++ = ' ';
            *s++ = str[0];
            *s++ = '\0';
          }
          but = uiDefBut(block,
                         UI_BTYPE_LABEL,
                         0,
                         use_prefix ? name_with_suffix : str,
                         0,
                         0,
                         w,
                         UI_UNIT_Y,
                         nullptr,
                         0.0,
                         0.0,
                         "");
          but->drawflag |= UI_BUT_TEXT_RIGHT;
          but->drawflag &= ~UI_BUT_TEXT_LEFT;

          label_added = true;
        }
      }
      else {
        but = uiDefBut(block, UI_BTYPE_LABEL, 0, name, 0, 0, w, UI_UNIT_Y, nullptr, 0.0, 0.0, "");
        but->drawflag |= UI_BUT_TEXT_RIGHT;
        but->drawflag &= ~UI_BUT_TEXT_LEFT;

        label_added = true;
      }

      if (!label_added && heading_layout) {
        ui_layout_heading_label_add(layout_sub, heading_layout, true, false);
      }

      layout_split = ui_item_prop_split_layout_hack(layout_parent, layout_split);

      /* Watch out! We can only write into the new layout now. */
      if ((type == PROP_ENUM) && (flag & UI_ITEM_R_EXPAND)) {
        /* Expanded enums each have their own name. */

        /* Often expanded enum's are better arranged into a row,
         * so check the existing layout. */
        if (uiLayoutGetLocalDir(layout) == UI_LAYOUT_HORIZONTAL) {
          layout = uiLayoutRow(layout_split, true);
        }
        else {
          layout = uiLayoutColumn(layout_split, true);
        }
      }
      else {
        if (use_prop_sep_split_label) {
          name = "";
        }
        layout = uiLayoutColumn(layout_split, true);
      }
      layout->space = 0;
    }

#ifdef UI_PROP_DECORATE
    if (ui_decorate.use_prop_decorate) {
      ui_decorate.layout = uiLayoutColumn(layout_row, true);
      ui_decorate.layout->space = 0;
      UI_block_layout_set_current(block, layout);
      ui_decorate.but = block->last_but();

      /* Clear after. */
      layout->flag |= uiItemInternalFlag::PropDecorateNoPad;
    }
#endif /* UI_PROP_DECORATE */
  }
  /* End split. */
  else if (heading_layout) {
    /* Could not add heading to split layout, fallback to inserting it to the layout with the
     * heading itself. */
    ui_layout_heading_label_add(heading_layout, heading_layout, false, false);
  }

  /* array property */
  if (index == RNA_NO_INDEX && is_array) {
    if (inside_prop_sep) {
      /* Within a split row, add array items to a column so they match the column layout of
       * previous items (e.g. transform vector with lock icon for each item). */
      layout = uiLayoutColumn(layout, true);
    }

    ui_item_array(layout,
                  block,
                  name,
                  icon,
                  ptr,
                  prop,
                  len,
                  0,
                  0,
                  w,
                  h,
                  expand,
                  slider,
                  toggle,
                  icon_only,
                  compact,
                  !use_prop_sep_split_label);
  }
  /* enum item */
  else if (type == PROP_ENUM && index == RNA_ENUM_VALUE) {
    if (icon && !name.is_empty() && !icon_only) {
      uiDefIconTextButR_prop(
          block, UI_BTYPE_ROW, 0, icon, name, 0, 0, w, h, ptr, prop, -1, 0, value, std::nullopt);
    }
    else if (icon) {
      uiDefIconButR_prop(
          block, UI_BTYPE_ROW, 0, icon, 0, 0, w, h, ptr, prop, -1, 0, value, std::nullopt);
    }
    else {
      uiDefButR_prop(
          block, UI_BTYPE_ROW, 0, name, 0, 0, w, h, ptr, prop, -1, 0, value, std::nullopt);
    }
  }
  /* expanded enum */
  else if (type == PROP_ENUM && expand) {
    ui_item_enum_expand(layout, block, ptr, prop, name, h, icon_only);
  }
  /* property with separate label */
  else if (ELEM(type, PROP_ENUM, PROP_STRING, PROP_POINTER)) {
    but = ui_item_with_label(layout, block, name, icon, ptr, prop, index, 0, 0, w, h, flag);

    if (is_id_name_prop) {
      Main *bmain = CTX_data_main(static_cast<bContext *>(block->evil_C));
      ID *id = ptr->owner_id;
      UI_but_func_rename_full_set(
          but, [bmain, id](const std::string &new_name) { ED_id_rename(*bmain, *id, new_name); });
    }

    bool results_are_suggestions = false;
    if (type == PROP_STRING) {
      const eStringPropertySearchFlag search_flag = RNA_property_string_search_flag(prop);
      if (search_flag & PROP_STRING_SEARCH_SUGGESTION) {
        results_are_suggestions = true;
      }
    }
    but = ui_but_add_search(but, ptr, prop, nullptr, nullptr, results_are_suggestions);

    if (layout->redalert) {
      UI_but_flag_enable(but, UI_BUT_REDALERT);
    }

    if (layout->activate_init) {
      UI_but_flag_enable(but, UI_BUT_ACTIVATE_ON_INIT);
    }
  }
  /* single button */
  else {
    but = uiDefAutoButR(block, ptr, prop, index, name, icon, 0, 0, w, h);

    if (slider && but->type == UI_BTYPE_NUM) {
      uiButNumber *number_but = (uiButNumber *)but;
      const float step_size = number_but->step_size;
      const float precision = number_but->precision;
      but = ui_but_change_type(but, UI_BTYPE_NUM_SLIDER);
      uiButNumberSlider *slider_but = reinterpret_cast<uiButNumberSlider *>(but);
      slider_but->step_size = step_size;
      slider_but->precision = precision;
    }

    if (flag & UI_ITEM_R_CHECKBOX_INVERT) {
      if (ELEM(but->type,
               UI_BTYPE_CHECKBOX,
               UI_BTYPE_CHECKBOX_N,
               UI_BTYPE_ICON_TOGGLE,
               UI_BTYPE_ICON_TOGGLE_N))
      {
        but->drawflag |= UI_BUT_CHECKBOX_INVERT;
      }
    }

    if ((toggle == 1) && but->type == UI_BTYPE_CHECKBOX) {
      but->type = UI_BTYPE_TOGGLE;
    }

    if (layout->redalert) {
      UI_but_flag_enable(but, UI_BUT_REDALERT);
    }

    if (layout->activate_init) {
      UI_but_flag_enable(but, UI_BUT_ACTIVATE_ON_INIT);
    }
  }

  /* The resulting button may have the icon set since boolean button drawing
   * is being 'helpful' and adding an icon for us.
   * In this case we want the ability not to have an icon.
   *
   * We could pass an argument not to set the icon to begin with however this is the one case
   * the functionality is needed. */
  if (but && no_icon) {
    if ((icon == ICON_NONE) && (but->icon != ICON_NONE)) {
      ui_def_but_icon_clear(but);
    }
  }

  /* Mark non-embossed text-fields inside a list-box. */
  if (but && (block->flag & UI_BLOCK_LIST_ITEM) && (but->type == UI_BTYPE_TEXT) &&
      ELEM(but->emboss, blender::ui::EmbossType::None, blender::ui::EmbossType::NoneOrStatus))
  {
    UI_but_flag_enable(but, UI_BUT_LIST_ITEM);
  }

  if (but) {
    if (placeholder) {
      UI_but_placeholder_set(but, placeholder->c_str());
    }
    if (ELEM(but->type, UI_BTYPE_TEXT) && (flag & UI_ITEM_R_TEXT_BUT_FORCE_SEMI_MODAL_ACTIVE)) {
      UI_but_flag2_enable(but, UI_BUT2_FORCE_SEMI_MODAL_ACTIVE);
    }
  }

#ifdef UI_PROP_DECORATE
  if (ui_decorate.use_prop_decorate) {
    uiBut *but_decorate = ui_decorate.but ? block->next_but(ui_decorate.but) : block->first_but();

    /* Move temporarily last buts to avoid multiple reallocations while inserting decorators. */
    blender::Vector<std::unique_ptr<uiBut>> tmp;
    tmp.reserve(ui_decorate.len);
    while (but_decorate && but_decorate != block->buttons.last().get()) {
      tmp.append(block->buttons.pop_last());
    }
    const bool use_blank_decorator = (flag & UI_ITEM_R_FORCE_BLANK_DECORATE);
    uiLayout *layout_col = uiLayoutColumn(ui_decorate.layout, false);
    layout_col->space = 0;
    layout_col->emboss = blender::ui::EmbossType::None;

    int i;
    for (i = 0; i < ui_decorate.len && but_decorate; i++) {
      PointerRNA *ptr_dec = use_blank_decorator ? nullptr : &but_decorate->rnapoin;
      PropertyRNA *prop_dec = use_blank_decorator ? nullptr : but_decorate->rnaprop;

      /* The icons are set in 'ui_but_anim_flag' */
      uiItemDecoratorR_prop(layout_col, ptr_dec, prop_dec, but_decorate->rnaindex);
      but = block->buttons.last().get();

      if (!tmp.is_empty()) {
        block->buttons.append(tmp.pop_last());
        but_decorate = block->buttons.last().get();
      }
      else {
        but_decorate = nullptr;
      }
    }
    while (!tmp.is_empty()) {
      block->buttons.append(tmp.pop_last());
    }
    BLI_assert(ELEM(i, 1, ui_decorate.len));

    layout->flag &= ~uiItemInternalFlag::PropDecorateNoPad;
  }
#endif /* UI_PROP_DECORATE */

  if (no_bg) {
    layout->emboss = prev_emboss;
  }

  /* ensure text isn't added to icon_only buttons */
  if (but && icon_only) {
    BLI_assert(but->str.empty());
  }
}

void uiItemR(uiLayout *layout,
             PointerRNA *ptr,
             const StringRefNull propname,
             const eUI_Item_Flag flag,
             const std::optional<StringRefNull> name,
             int icon)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());

  if (!prop) {
    ui_item_disabled(layout, propname.c_str());
    RNA_warning("property not found: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }

  uiItemFullR(layout, ptr, prop, RNA_NO_INDEX, 0, flag, name, icon);
}

void uiItemFullR_with_popover(uiLayout *layout,
                              PointerRNA *ptr,
                              PropertyRNA *prop,
                              int index,
                              int value,
                              const eUI_Item_Flag flag,
                              const std::optional<StringRefNull> name,
                              int icon,
                              const char *panel_type)
{
  uiBlock *block = layout->root->block;
  int i = block->buttons.size();
  uiItemFullR(layout, ptr, prop, index, value, flag, name, icon);
  for (; i < block->buttons.size(); i++) {
    uiBut *but = block->buttons[i].get();
    if (but->rnaprop == prop && ELEM(but->type, UI_BTYPE_MENU, UI_BTYPE_COLOR)) {
      ui_but_rna_menu_convert_to_panel_type(but, panel_type);
      break;
    }
  }
  if (i == block->buttons.size()) {
    const StringRefNull propname = RNA_property_identifier(prop);
    ui_item_disabled(layout, panel_type);
    RNA_warning("property could not use a popover: %s.%s (%s)",
                RNA_struct_identifier(ptr->type),
                propname.c_str(),
                panel_type);
  }
}

void uiItemFullR_with_menu(uiLayout *layout,
                           PointerRNA *ptr,
                           PropertyRNA *prop,
                           int index,
                           int value,
                           const eUI_Item_Flag flag,
                           const std::optional<StringRefNull> name,
                           int icon,
                           const char *menu_type)
{
  uiBlock *block = layout->root->block;
  int i = block->buttons.size();
  uiItemFullR(layout, ptr, prop, index, value, flag, name, icon);
  while (i < block->buttons.size()) {
    uiBut *but = block->buttons[i].get();
    if (but->rnaprop == prop && but->type == UI_BTYPE_MENU) {
      ui_but_rna_menu_convert_to_menu_type(but, menu_type);
      break;
    }
    i++;
  }
  if (i == block->buttons.size()) {
    const StringRefNull propname = RNA_property_identifier(prop);
    ui_item_disabled(layout, menu_type);
    RNA_warning("property could not use a menu: %s.%s (%s)",
                RNA_struct_identifier(ptr->type),
                propname.c_str(),
                menu_type);
  }
}

void uiItemEnumR_prop(uiLayout *layout,
                      const std::optional<StringRefNull> name,
                      int icon,
                      PointerRNA *ptr,
                      PropertyRNA *prop,
                      int value)
{
  if (RNA_property_type(prop) != PROP_ENUM) {
    const StringRefNull propname = RNA_property_identifier(prop);
    ui_item_disabled(layout, propname.c_str());
    RNA_warning("property not an enum: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }

  uiItemFullR(layout, ptr, prop, RNA_ENUM_VALUE, value, UI_ITEM_NONE, name, icon);
}

void uiItemEnumR_string_prop(uiLayout *layout,
                             PointerRNA *ptr,
                             PropertyRNA *prop,
                             const char *value,
                             const std::optional<StringRefNull> name,
                             int icon)
{
  if (UNLIKELY(RNA_property_type(prop) != PROP_ENUM)) {
    const StringRefNull propname = RNA_property_identifier(prop);
    ui_item_disabled(layout, propname.c_str());
    RNA_warning("not an enum property: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }

  const EnumPropertyItem *item;
  bool free;
  RNA_property_enum_items(
      static_cast<bContext *>(layout->root->block->evil_C), ptr, prop, &item, nullptr, &free);

  int ivalue;
  if (!RNA_enum_value_from_id(item, value, &ivalue)) {
    const StringRefNull propname = RNA_property_identifier(prop);
    if (free) {
      MEM_freeN((void *)item);
    }
    ui_item_disabled(layout, propname.c_str());
    RNA_warning("enum property value not found: %s", value);
    return;
  }

  for (int a = 0; item[a].identifier; a++) {
    if (item[a].identifier[0] == '\0') {
      /* Skip enum item separators. */
      continue;
    }
    if (item[a].value == ivalue) {
      const StringRefNull item_name = name.value_or(
          CTX_IFACE_(RNA_property_translation_context(prop), item[a].name));
      const eUI_Item_Flag flag = !item_name.is_empty() ? UI_ITEM_NONE : UI_ITEM_R_ICON_ONLY;

      uiItemFullR(
          layout, ptr, prop, RNA_ENUM_VALUE, ivalue, flag, item_name, icon ? icon : item[a].icon);
      break;
    }
  }

  if (free) {
    MEM_freeN((void *)item);
  }
}

void uiItemEnumR_string(uiLayout *layout,
                        PointerRNA *ptr,
                        const StringRefNull propname,
                        const char *value,
                        const std::optional<StringRefNull> name,
                        int icon)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  if (UNLIKELY(prop == nullptr)) {
    ui_item_disabled(layout, propname.c_str());
    RNA_warning(
        "enum property not found: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }
  uiItemEnumR_string_prop(layout, ptr, prop, value, name, icon);
}

void uiItemsEnumR(uiLayout *layout, PointerRNA *ptr, const StringRefNull propname)
{
  uiBlock *block = layout->root->block;

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());

  if (!prop) {
    ui_item_disabled(layout, propname.c_str());
    RNA_warning(
        "enum property not found: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }

  if (RNA_property_type(prop) != PROP_ENUM) {
    RNA_warning("not an enum property: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }

  uiLayout *split = uiLayoutSplit(layout, 0.0f, false);
  uiLayout *column = uiLayoutColumn(split, false);

  int totitem;
  const EnumPropertyItem *item;
  bool free;
  RNA_property_enum_items_gettexted(
      static_cast<bContext *>(block->evil_C), ptr, prop, &item, &totitem, &free);

  for (int i = 0; i < totitem; i++) {
    if (item[i].identifier[0]) {
      uiItemEnumR_prop(column, item[i].name, item[i].icon, ptr, prop, item[i].value);
      ui_but_tip_from_enum_item(block->buttons.last().get(), &item[i]);
    }
    else {
      if (item[i].name) {
        if (i != 0) {
          column = uiLayoutColumn(split, false);
        }

        uiItemL(column, item[i].name, ICON_NONE);
        uiBut *bt = block->buttons.last().get();
        bt->drawflag = UI_BUT_TEXT_LEFT;

        ui_but_tip_from_enum_item(bt, &item[i]);
      }
      else {
        uiItemS(column);
      }
    }
  }

  if (free) {
    MEM_freeN((void *)item);
  }
}

/* Pointer RNA button with search */

static void search_id_collection(StructRNA *ptype, PointerRNA *r_ptr, PropertyRNA **r_prop)
{
  /* look for collection property in Main */
  /* NOTE: using global Main is OK-ish here, UI shall not access other Mains anyway. */
  *r_ptr = RNA_main_pointer_create(G_MAIN);

  *r_prop = nullptr;

  RNA_STRUCT_BEGIN (r_ptr, iprop) {
    /* if it's a collection and has same pointer type, we've got it */
    if (RNA_property_type(iprop) == PROP_COLLECTION) {
      StructRNA *srna = RNA_property_pointer_type(r_ptr, iprop);

      if (ptype == srna) {
        *r_prop = iprop;
        break;
      }
    }
  }
  RNA_STRUCT_END;
}

static void ui_rna_collection_search_arg_free_fn(void *ptr)
{
  uiRNACollectionSearch *coll_search = static_cast<uiRNACollectionSearch *>(ptr);
  UI_butstore_free(coll_search->butstore_block, coll_search->butstore);
  MEM_delete(coll_search);
}

uiBut *ui_but_add_search(uiBut *but,
                         PointerRNA *ptr,
                         PropertyRNA *prop,
                         PointerRNA *searchptr,
                         PropertyRNA *searchprop,
                         const bool results_are_suggestions)
{
  /* for ID's we do automatic lookup */
  bool has_search_fn = false;

  PointerRNA sptr;
  if (!searchprop) {
    if (RNA_property_type(prop) == PROP_STRING) {
      has_search_fn = (RNA_property_string_search_flag(prop) != 0);
    }
    if (RNA_property_type(prop) == PROP_POINTER) {
      StructRNA *ptype = RNA_property_pointer_type(ptr, prop);
      search_id_collection(ptype, &sptr, &searchprop);
      searchptr = &sptr;
    }
  }

  /* turn button into search button */
  if (has_search_fn || searchprop) {
    uiRNACollectionSearch *coll_search = MEM_new<uiRNACollectionSearch>(__func__);
    uiButSearch *search_but;

    but = ui_but_change_type(but, UI_BTYPE_SEARCH_MENU);
    search_but = (uiButSearch *)but;

    if (searchptr) {
      search_but->rnasearchpoin = *searchptr;
      search_but->rnasearchprop = searchprop;
    }

    but->hardmax = std::max(but->hardmax, 256.0f);
    but->drawflag |= UI_BUT_ICON_LEFT | UI_BUT_TEXT_LEFT;
    if (RNA_property_is_unlink(prop)) {
      but->flag |= UI_BUT_VALUE_CLEAR;
    }

    coll_search->target_ptr = *ptr;
    coll_search->target_prop = prop;

    if (searchptr) {
      coll_search->search_ptr = *searchptr;
      coll_search->search_prop = searchprop;
    }
    else {
      /* Rely on `has_search_fn`. */
      coll_search->search_ptr = PointerRNA_NULL;
      coll_search->search_prop = nullptr;
    }

    coll_search->search_but = but;
    coll_search->butstore_block = but->block;
    coll_search->butstore = UI_butstore_create(coll_search->butstore_block);
    UI_butstore_register(coll_search->butstore, &coll_search->search_but);

    if (RNA_property_type(prop) == PROP_ENUM) {
      /* XXX, this will have a menu string,
       * but in this case we just want the text */
      but->str.clear();
    }

    UI_but_func_search_set_results_are_suggestions(but, results_are_suggestions);

    UI_but_func_search_set(but,
                           ui_searchbox_create_generic,
                           ui_rna_collection_search_update_fn,
                           coll_search,
                           false,
                           ui_rna_collection_search_arg_free_fn,
                           nullptr,
                           nullptr);
    /* If this is called multiple times for the same button, an earlier call may have taken the
     * else branch below so the button was disabled. Now we have a searchprop, so it can be enabled
     * again. */
    but->flag &= ~UI_BUT_DISABLED;
  }
  else if (but->type == UI_BTYPE_SEARCH_MENU) {
    /* In case we fail to find proper searchprop,
     * so other code might have already set but->type to search menu... */
    but->flag |= UI_BUT_DISABLED;
  }

  return but;
}

void uiItemPointerR_prop(uiLayout *layout,
                         PointerRNA *ptr,
                         PropertyRNA *prop,
                         PointerRNA *searchptr,
                         PropertyRNA *searchprop,
                         const std::optional<StringRefNull> name_opt,
                         int icon,
                         bool results_are_suggestions)
{
  const bool use_prop_sep = bool(layout->flag & uiItemInternalFlag::PropSep);

  ui_block_new_button_group(uiLayoutGetBlock(layout), uiButtonGroupFlag(0));

  const PropertyType type = RNA_property_type(prop);
  if (!ELEM(type, PROP_POINTER, PROP_STRING, PROP_ENUM)) {
    RNA_warning("Property %s.%s must be a pointer, string or enum",
                RNA_struct_identifier(ptr->type),
                RNA_property_identifier(prop));
    return;
  }
  if (RNA_property_type(searchprop) != PROP_COLLECTION) {
    RNA_warning("search collection property is not a collection type: %s.%s",
                RNA_struct_identifier(searchptr->type),
                RNA_property_identifier(searchprop));
    return;
  }

  /* get icon & name */
  if (icon == ICON_NONE) {
    const StructRNA *icontype;
    if (type == PROP_POINTER) {
      icontype = RNA_property_pointer_type(ptr, prop);
    }
    else {
      icontype = RNA_property_pointer_type(searchptr, searchprop);
    }

    icon = RNA_struct_ui_icon(icontype);
  }
  StringRefNull name = name_opt.value_or(RNA_property_ui_name(prop));

  char namestr[UI_MAX_NAME_STR];
  if (use_prop_sep == false) {
    name = ui_item_name_add_colon(name, namestr);
  }

  /* create button */
  uiBlock *block = uiLayoutGetBlock(layout);

  int w, h;
  ui_item_rna_size(layout, name, icon, ptr, prop, 0, false, false, &w, &h);
  w += UI_UNIT_X; /* X icon needs more space */
  uiBut *but = ui_item_with_label(layout, block, name, icon, ptr, prop, 0, 0, 0, w, h, 0);

  but = ui_but_add_search(but, ptr, prop, searchptr, searchprop, results_are_suggestions);
}

void uiItemPointerR(uiLayout *layout,
                    PointerRNA *ptr,
                    const StringRefNull propname,
                    PointerRNA *searchptr,
                    const StringRefNull searchpropname,
                    const std::optional<StringRefNull> name,
                    int icon)
{
  /* validate arguments */
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  if (!prop) {
    RNA_warning("property not found: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }
  PropertyRNA *searchprop = RNA_struct_find_property(searchptr, searchpropname.c_str());
  if (!searchprop) {
    RNA_warning("search collection property not found: %s.%s",
                RNA_struct_identifier(searchptr->type),
                searchpropname.c_str());
    return;
  }

  uiItemPointerR_prop(layout, ptr, prop, searchptr, searchprop, name, icon, false);
}

void ui_item_menutype_func(bContext *C, uiLayout *layout, void *arg_mt)
{
  MenuType *mt = (MenuType *)arg_mt;
  UI_menutype_draw(C, mt, layout);
}

void ui_item_paneltype_func(bContext *C, uiLayout *layout, void *arg_pt)
{
  PanelType *pt = (PanelType *)arg_pt;
  UI_paneltype_draw(C, pt, layout);
}

static uiBut *ui_item_menu(uiLayout *layout,
                           const StringRef name,
                           int icon,
                           uiMenuCreateFunc func,
                           void *arg,
                           void *argN,
                           const std::optional<StringRef> tip,
                           bool force_menu,
                           uiButArgNFree func_argN_free_fn = MEM_freeN,
                           uiButArgNCopy func_argN_copy_fn = MEM_dupallocN)
{
  uiBlock *block = layout->root->block;
  uiLayout *heading_layout = ui_layout_heading_find(layout);

  UI_block_layout_set_current(block, layout);
  ui_block_new_button_group(block, uiButtonGroupFlag(0));

  if (layout->root->type == UI_LAYOUT_MENU && !icon) {
    icon = ICON_BLANK1;
  }

  uiTextIconPadFactor pad_factor = ui_text_pad_compact;
  if (layout->root->type == UI_LAYOUT_HEADER) { /* Ugly! */
    if (icon == ICON_NONE && force_menu) {
      /* pass */
    }
    else if (force_menu) {
      pad_factor.text = 1.85;
      pad_factor.icon_only = 0.6f;
    }
    else {
      pad_factor.text = 0.75f;
    }
  }

  const int w = ui_text_icon_width_ex(layout, name, icon, pad_factor, UI_FSTYLE_WIDGET);
  const int h = UI_UNIT_Y;

  if (heading_layout) {
    ui_layout_heading_label_add(layout, heading_layout, true, true);
  }

  uiBut *but;
  if (!name.is_empty() && icon) {
    but = uiDefIconTextMenuBut(block, func, arg, icon, name, 0, 0, w, h, tip);
  }
  else if (icon) {
    but = uiDefIconMenuBut(block, func, arg, icon, 0, 0, w, h, tip);
    if (force_menu && !name.is_empty()) {
      UI_but_drawflag_enable(but, UI_BUT_ICON_LEFT);
    }
  }
  else {
    but = uiDefMenuBut(block, func, arg, name, 0, 0, w, h, tip);
  }

  if (argN) {
    /* ugly! */
    if (arg != argN) {
      but->poin = (char *)but;
    }
    but->func_argN = argN;
    but->func_argN_free_fn = func_argN_free_fn;
    but->func_argN_copy_fn = func_argN_copy_fn;
  }

  if (ELEM(layout->root->type, UI_LAYOUT_PANEL, UI_LAYOUT_TOOLBAR) ||
      /* We never want a drop-down in menu! */
      (force_menu && layout->root->type != UI_LAYOUT_MENU))
  {
    UI_but_type_set_menu_from_pulldown(but);
  }

  return but;
}

void uiItemM_ptr(uiLayout *layout, MenuType *mt, const std::optional<StringRef> name_opt, int icon)
{
  uiBlock *block = layout->root->block;
  bContext *C = static_cast<bContext *>(block->evil_C);
  if (WM_menutype_poll(C, mt) == false) {
    return;
  }

  const StringRef name = name_opt.value_or(CTX_IFACE_(mt->translation_context, mt->label));

  if (layout->root->type == UI_LAYOUT_MENU && !icon) {
    icon = ICON_BLANK1;
  }

  ui_item_menu(layout,
               name,
               icon,
               ui_item_menutype_func,
               mt,
               nullptr,
               mt->description ? TIP_(mt->description) : "",
               false);
}

void uiItemM(uiLayout *layout,
             const StringRef menuname,
             const std::optional<StringRef> name,
             int icon)
{
  MenuType *mt = WM_menutype_find(menuname, false);
  if (mt == nullptr) {
    RNA_warning("not found %s", std::string(menuname).c_str());
    return;
  }
  uiItemM_ptr(layout, mt, name, icon);
}

void uiItemMContents(uiLayout *layout, const StringRef menuname)
{
  MenuType *mt = WM_menutype_find(menuname, false);
  if (mt == nullptr) {
    RNA_warning("not found %s", std::string(menuname).c_str());
    return;
  }

  uiBlock *block = layout->root->block;
  bContext *C = static_cast<bContext *>(block->evil_C);
  if (WM_menutype_poll(C, mt) == false) {
    return;
  }

  const bContextStore *previous_ctx = CTX_store_get(C);
  UI_menutype_draw(C, mt, layout);

  /* Restore context that was cleared by `UI_menutype_draw`. */
  if (layout->context) {
    CTX_store_set(C, previous_ctx);
  }
}

void uiItemDecoratorR_prop(uiLayout *layout, PointerRNA *ptr, PropertyRNA *prop, int index)
{
  uiBlock *block = layout->root->block;

  UI_block_layout_set_current(block, layout);
  uiLayout *col = uiLayoutColumn(layout, false);
  col->space = 0;
  col->emboss = blender::ui::EmbossType::None;

  if (ELEM(nullptr, ptr, prop) || !RNA_property_animateable(ptr, prop)) {
    uiBut *but = uiDefIconBut(block,
                              UI_BTYPE_DECORATOR,
                              0,
                              ICON_BLANK1,
                              0,
                              0,
                              UI_UNIT_X,
                              UI_UNIT_Y,
                              nullptr,
                              0.0,
                              0.0,
                              "");
    but->flag |= UI_BUT_DISABLED;
    return;
  }

  const bool is_expand = ui_item_rna_is_expand(prop, index, UI_ITEM_NONE);
  const bool is_array = RNA_property_array_check(prop);

  /* Loop for the array-case, but only do in case of an expanded array. */
  for (int i = 0; i < (is_expand ? RNA_property_array_length(ptr, prop) : 1); i++) {
    uiButDecorator *but = (uiButDecorator *)uiDefIconBut(block,
                                                         UI_BTYPE_DECORATOR,
                                                         0,
                                                         ICON_DOT,
                                                         0,
                                                         0,
                                                         UI_UNIT_X,
                                                         UI_UNIT_Y,
                                                         nullptr,
                                                         0.0,
                                                         0.0,
                                                         TIP_("Animate property"));

    UI_but_func_set(but, ui_but_anim_decorate_cb, but, nullptr);
    but->flag |= UI_BUT_UNDO | UI_BUT_DRAG_LOCK;
    /* Decorators have their own RNA data, using the normal #uiBut RNA members has many
     * side-effects. */
    but->decorated_rnapoin = *ptr;
    but->decorated_rnaprop = prop;
    /* ui_def_but_rna() sets non-array buttons to have a RNA index of 0. */
    but->decorated_rnaindex = (!is_array || is_expand) ? i : index;
  }
}

void uiItemDecoratorR(uiLayout *layout,
                      PointerRNA *ptr,
                      const std::optional<StringRefNull> propname,
                      int index)
{
  PropertyRNA *prop = nullptr;

  if (ptr && propname) {
    /* validate arguments */
    prop = RNA_struct_find_property(ptr, propname->c_str());
    if (!prop) {
      ui_item_disabled(layout, propname->c_str());
      RNA_warning(
          "property not found: %s.%s", RNA_struct_identifier(ptr->type), propname->c_str());
      return;
    }
  }

  /* ptr and prop are allowed to be nullptr here. */
  uiItemDecoratorR_prop(layout, ptr, prop, index);
}

void uiItemPopoverPanel_ptr(uiLayout *layout,
                            const bContext *C,
                            PanelType *pt,
                            const std::optional<StringRef> name_opt,
                            int icon)
{
  const StringRef name = name_opt.value_or(CTX_IFACE_(pt->translation_context, pt->label));

  if (layout->root->type == UI_LAYOUT_MENU && !icon) {
    icon = ICON_BLANK1;
  }

  const bContextStore *previous_ctx = CTX_store_get(C);
  /* Set context for polling (and panel header drawing). */
  CTX_store_set(const_cast<bContext *>(C), layout->context);

  const bool ok = (pt->poll == nullptr) || pt->poll(C, pt);
  if (ok && (pt->draw_header != nullptr)) {
    layout = uiLayoutRow(layout, true);
    Panel panel{};
    Panel_Runtime panel_runtime{};
    panel.runtime = &panel_runtime;
    panel.type = pt;
    panel.layout = layout;
    panel.flag = PNL_POPOVER;
    pt->draw_header(C, &panel);
  }

  CTX_store_set(const_cast<bContext *>(C), previous_ctx);

  uiBut *but = ui_item_menu(
      layout, name, icon, ui_item_paneltype_func, pt, nullptr, TIP_(pt->description), true);
  but->type = UI_BTYPE_POPOVER;
  if (!ok) {
    but->flag |= UI_BUT_DISABLED;
  }
}

void uiItemPopoverPanel(uiLayout *layout,
                        const bContext *C,
                        const StringRef panel_type,
                        std::optional<blender::StringRef> name_opt,
                        int icon)
{
  PanelType *pt = WM_paneltype_find(panel_type, true);
  if (pt == nullptr) {
    RNA_warning("Panel type not found '%s'", std::string(panel_type).c_str());
    return;
  }
  uiItemPopoverPanel_ptr(layout, C, pt, name_opt, icon);
}

void uiItemPopoverPanelFromGroup(uiLayout *layout,
                                 bContext *C,
                                 int space_id,
                                 int region_id,
                                 const char *context,
                                 const char *category)
{
  SpaceType *st = BKE_spacetype_from_id(space_id);
  if (st == nullptr) {
    RNA_warning("space type not found %d", space_id);
    return;
  }
  ARegionType *art = BKE_regiontype_from_id(st, region_id);
  if (art == nullptr) {
    RNA_warning("region type not found %d", region_id);
    return;
  }

  LISTBASE_FOREACH (PanelType *, pt, &art->paneltypes) {
    /* Causes too many panels, check context. */
    if (pt->parent_id[0] == '\0') {
      if (/* (*context == '\0') || */ STREQ(pt->context, context)) {
        if ((*category == '\0') || STREQ(pt->category, category)) {
          if (pt->poll == nullptr || pt->poll(C, pt)) {
            uiItemPopoverPanel_ptr(layout, C, pt, std::nullopt, ICON_NONE);
          }
        }
      }
    }
  }
}

/* label item */
static uiBut *uiItemL_(uiLayout *layout, const StringRef name, int icon)
{
  uiBlock *block = layout->root->block;

  UI_block_layout_set_current(block, layout);
  ui_block_new_button_group(block, uiButtonGroupFlag(0));

  if (layout->root->type == UI_LAYOUT_MENU && !icon) {
    icon = ICON_BLANK1;
  }

  const int w = ui_text_icon_width_ex(layout, name, icon, ui_text_pad_none, UI_FSTYLE_WIDGET);
  uiBut *but;
  if (icon && !name.is_empty()) {
    but = uiDefIconTextBut(
        block, UI_BTYPE_LABEL, 0, icon, name, 0, 0, w, UI_UNIT_Y, nullptr, 0.0, 0.0, std::nullopt);
  }
  else if (icon) {
    but = uiDefIconBut(
        block, UI_BTYPE_LABEL, 0, icon, 0, 0, w, UI_UNIT_Y, nullptr, 0.0, 0.0, std::nullopt);
  }
  else {
    but = uiDefBut(
        block, UI_BTYPE_LABEL, 0, name, 0, 0, w, UI_UNIT_Y, nullptr, 0.0, 0.0, std::nullopt);
  }

  /* to compensate for string size padding in ui_text_icon_width,
   * make text aligned right if the layout is aligned right.
   */
  if (uiLayoutGetAlignment(layout) == UI_LAYOUT_ALIGN_RIGHT) {
    but->drawflag &= ~UI_BUT_TEXT_LEFT; /* default, needs to be unset */
    but->drawflag |= UI_BUT_TEXT_RIGHT;
  }

  /* Mark as a label inside a list-box. */
  if (block->flag & UI_BLOCK_LIST_ITEM) {
    but->flag |= UI_BUT_LIST_ITEM;
  }

  if (layout->redalert) {
    UI_but_flag_enable(but, UI_BUT_REDALERT);
  }

  return but;
}

uiBut *uiItemL_ex(
    uiLayout *layout, const StringRef name, int icon, const bool highlight, const bool redalert)
{
  uiBut *but = uiItemL_(layout, name, icon);

  if (highlight) {
    /* TODO: add another flag for this. */
    UI_but_flag_enable(but, UI_SELECT_DRAW);
  }

  if (redalert) {
    UI_but_flag_enable(but, UI_BUT_REDALERT);
  }

  return but;
}

void uiItemL(uiLayout *layout, const StringRef name, int icon)
{
  uiItemL_(layout, name, icon);
}

uiPropertySplitWrapper uiItemPropertySplitWrapperCreate(uiLayout *parent_layout)
{
  uiPropertySplitWrapper split_wrapper = {nullptr};

  uiLayout *layout_row = uiLayoutRow(parent_layout, true);
  uiLayout *layout_split = uiLayoutSplit(layout_row, UI_ITEM_PROP_SEP_DIVIDE, true);

  split_wrapper.label_column = uiLayoutColumn(layout_split, true);
  split_wrapper.label_column->alignment = UI_LAYOUT_ALIGN_RIGHT;
  split_wrapper.property_row = ui_item_prop_split_layout_hack(parent_layout, layout_split);
  split_wrapper.decorate_column = uiLayoutGetPropDecorate(parent_layout) ?
                                      uiLayoutColumn(layout_row, true) :
                                      nullptr;

  return split_wrapper;
}

uiLayout *uiItemL_respect_property_split(uiLayout *layout, StringRef text, int icon)
{
  if (bool(layout->flag & uiItemInternalFlag::PropSep)) {
    uiBlock *block = uiLayoutGetBlock(layout);
    const uiPropertySplitWrapper split_wrapper = uiItemPropertySplitWrapperCreate(layout);
    /* Further items added to 'layout' will automatically be added to split_wrapper.property_row */

    uiItemL_(split_wrapper.label_column, text, icon);
    UI_block_layout_set_current(block, split_wrapper.property_row);

    return split_wrapper.decorate_column;
  }

  char namestr[UI_MAX_NAME_STR];
  text = ui_item_name_add_colon(text, namestr);
  uiItemL_(layout, text, icon);

  return nullptr;
}

void uiItemLDrag(uiLayout *layout, PointerRNA *ptr, StringRef name, int icon)
{
  uiBut *but = uiItemL_(layout, name, icon);

  if (ptr && ptr->type) {
    if (RNA_struct_is_ID(ptr->type)) {
      UI_but_drag_set_id(but, ptr->owner_id);
    }
  }
}

void uiItemS_ex(uiLayout *layout, float factor, const LayoutSeparatorType type)
{
  uiBlock *block = layout->root->block;
  const bool is_menu = ui_block_is_menu(block);
  const bool is_pie = ui_block_is_pie_menu(block);
  if (is_menu && !UI_block_can_add_separator(block)) {
    return;
  }

  /* Sizing of spaces should not depend on line width. */
  const int space = (is_menu) ? int(7.0f * UI_SCALE_FAC * factor) :
                                int(6.0f * UI_SCALE_FAC * factor);

  eButType but_type;

  switch (type) {
    case LayoutSeparatorType::Line:
      but_type = UI_BTYPE_SEPR_LINE;
      break;
    case LayoutSeparatorType::Auto:
      but_type = (is_menu && !is_pie) ? UI_BTYPE_SEPR_LINE : UI_BTYPE_SEPR;
      break;
    default:
      but_type = UI_BTYPE_SEPR;
  }

  bool is_vertical_bar = (layout->w == 0) && but_type == UI_BTYPE_SEPR_LINE;

  UI_block_layout_set_current(block, layout);
  uiBut *but = uiDefBut(block,
                        but_type,
                        0,
                        "",
                        0,
                        0,
                        space,
                        is_vertical_bar ? UI_UNIT_Y : space,
                        nullptr,
                        0.0,
                        0.0,
                        "");

  if (but_type == UI_BTYPE_SEPR_LINE) {
    uiButSeparatorLine *but_line = static_cast<uiButSeparatorLine *>(but);
    but_line->is_vertical = is_vertical_bar;
  }
}

void uiItemS(uiLayout *layout)
{
  uiItemS_ex(layout, 1.0f);
}

void uiItemProgressIndicator(uiLayout *layout,
                             const char *text,
                             const float factor,
                             const eButProgressType progress_type)
{
  const bool has_text = text && text[0];
  uiBlock *block = layout->root->block;
  short width;

  if (progress_type == UI_BUT_PROGRESS_TYPE_BAR) {
    width = UI_UNIT_X * 5;
  }
  else if (has_text) {
    width = UI_UNIT_X * 8;
  }
  else {
    width = UI_UNIT_X;
  }

  UI_block_layout_set_current(block, layout);
  uiBut *but = uiDefBut(block,
                        UI_BTYPE_PROGRESS,
                        0,
                        (text) ? text : "",
                        0,
                        0,
                        width,
                        short(UI_UNIT_Y),
                        nullptr,
                        0.0,
                        0.0,
                        "");

  if (has_text && (progress_type == UI_BUT_PROGRESS_TYPE_RING)) {
    /* For progress bar, centered is okay, left aligned for ring/pie. */
    but->drawflag |= UI_BUT_TEXT_LEFT;
  }

  uiButProgress *progress_bar = static_cast<uiButProgress *>(but);
  progress_bar->progress_type = eButProgressType(progress_type);
  progress_bar->progress_factor = factor;
}

void uiItemSpacer(uiLayout *layout)
{
  uiBlock *block = layout->root->block;
  const bool is_popup = ui_block_is_popup_any(block);

  if (is_popup) {
    printf("Error: separator_spacer() not supported in popups.\n");
    return;
  }

  if (block->direction & UI_DIR_RIGHT) {
    printf("Error: separator_spacer() only supported in horizontal blocks.\n");
    return;
  }

  UI_block_layout_set_current(block, layout);
  uiDefBut(block,
           UI_BTYPE_SEPR_SPACER,
           0,
           "",
           0,
           0,
           0.3f * UI_UNIT_X,
           UI_UNIT_Y,
           nullptr,
           0.0,
           0.0,
           "");
}

void uiItemMenuF(
    uiLayout *layout, const StringRefNull name, int icon, uiMenuCreateFunc func, void *arg)
{
  if (!func) {
    return;
  }

  ui_item_menu(layout, name, icon, func, arg, nullptr, "", false);
}

void uiItemMenuFN(
    uiLayout *layout, const StringRefNull name, int icon, uiMenuCreateFunc func, void *argN)
{
  if (!func) {
    return;
  }

  /* Second 'argN' only ensures it gets freed. */
  ui_item_menu(layout, name, icon, func, argN, argN, "", false);
}

struct MenuItemLevel {
  wmOperatorCallContext opcontext;
  /* don't use pointers to the strings because python can dynamically
   * allocate strings and free before the menu draws, see #27304. */
  char opname[OP_MAX_TYPENAME];
  char propname[MAX_IDPROP_NAME];
  PointerRNA rnapoin;
};

/* Obtain the active menu item based on the calling button's text. */
static int menu_item_enum_opname_menu_active(bContext *C, uiBut *but, MenuItemLevel *lvl)
{
  wmOperatorType *ot = WM_operatortype_find(lvl->opname, true);

  if (!ot) {
    return -1;
  }

  PointerRNA ptr;
  const EnumPropertyItem *item_array = nullptr;
  bool free;
  int totitem;
  WM_operator_properties_create_ptr(&ptr, ot);
  /* so the context is passed to itemf functions (some need it) */
  WM_operator_properties_sanitize(&ptr, false);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, lvl->propname);
  if (!prop) {
    return -1;
  }
  RNA_property_enum_items_gettexted(C, &ptr, prop, &item_array, &totitem, &free);
  int active = RNA_enum_from_name(item_array, but->str.c_str());
  if (free) {
    MEM_freeN((void *)item_array);
  }

  return active;
}

static void menu_item_enum_opname_menu(bContext *C, uiLayout *layout, void *arg)
{
  uiBut *but = static_cast<uiBut *>(arg);
  MenuItemLevel *lvl = static_cast<MenuItemLevel *>(but->func_argN);
  /* Use the operator properties from the button owning the menu. */
  IDProperty *op_props = but->opptr ? static_cast<IDProperty *>(but->opptr->data) : nullptr;

  /* The calling but's str _probably_ contains the active
   * menu item name, set in uiItemMenuEnumFullO_ptr. */
  const int active = menu_item_enum_opname_menu_active(C, but, lvl);

  uiLayoutSetOperatorContext(layout, lvl->opcontext);
  uiItemsFullEnumO(
      layout, lvl->opname, lvl->propname, op_props, lvl->opcontext, UI_ITEM_NONE, active);

  /* override default, needed since this was assumed pre 2.70 */
  UI_block_direction_set(layout->root->block, UI_DIR_DOWN);
}

void uiItemMenuEnumFullO_ptr(uiLayout *layout,
                             const bContext *C,
                             wmOperatorType *ot,
                             const StringRefNull propname,
                             std::optional<StringRefNull> name,
                             int icon,
                             PointerRNA *r_opptr)
{
  /* Caller must check */
  BLI_assert(ot->srna != nullptr);

  std::string operator_name;
  if (!name) {
    operator_name = WM_operatortype_name(ot, nullptr);
    name = operator_name.c_str();
  }

  if (layout->root->type == UI_LAYOUT_MENU && !icon) {
    icon = ICON_BLANK1;
  }

  MenuItemLevel *lvl = MEM_new<MenuItemLevel>("MenuItemLevel");
  STRNCPY(lvl->opname, ot->idname);
  STRNCPY(lvl->propname, propname.c_str());
  lvl->opcontext = layout->root->opcontext;

  uiBut *but = ui_item_menu(layout,
                            *name,
                            icon,
                            menu_item_enum_opname_menu,
                            nullptr,
                            lvl,
                            std::nullopt,
                            true,
                            but_func_argN_free<MenuItemLevel>,
                            but_func_argN_copy<MenuItemLevel>);
  /* Use the menu button as owner for the operator properties, which will then be passed to the
   * individual menu items. */
  if (r_opptr) {
    but->opptr = MEM_new<PointerRNA>("uiButOpPtr");
    WM_operator_properties_create_ptr(but->opptr, ot);
    BLI_assert(but->opptr->data == nullptr);
    WM_operator_properties_alloc(&but->opptr, (IDProperty **)&but->opptr->data, ot->idname);
    *r_opptr = *but->opptr;
  }

  /* add hotkey here, lower UI code can't detect it */
  if ((layout->root->block->flag & UI_BLOCK_LOOP) && (ot->prop && ot->invoke)) {
    if (std::optional<std::string> shortcut_str = WM_key_event_operator_string(
            C, ot->idname, layout->root->opcontext, nullptr, false))
    {
      ui_but_add_shortcut(but, shortcut_str->c_str(), false);
    }
  }
}

void uiItemMenuEnumFullO(uiLayout *layout,
                         const bContext *C,
                         const StringRefNull opname,
                         const StringRefNull propname,
                         StringRefNull name,
                         int icon,
                         PointerRNA *r_opptr)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */

  UI_OPERATOR_ERROR_RET(ot, opname.c_str(), return);

  if (!ot->srna) {
    ui_item_disabled(layout, opname.c_str());
    RNA_warning("operator missing srna '%s'", opname.c_str());
    return;
  }

  uiItemMenuEnumFullO_ptr(layout, C, ot, propname, name, icon, r_opptr);
}

void uiItemMenuEnumO(uiLayout *layout,
                     const bContext *C,
                     const StringRefNull opname,
                     const StringRefNull propname,
                     StringRefNull name,
                     int icon)
{
  uiItemMenuEnumFullO(layout, C, opname, propname, name, icon, nullptr);
}

static void menu_item_enum_rna_menu(bContext * /*C*/, uiLayout *layout, void *arg)
{
  MenuItemLevel *lvl = (MenuItemLevel *)(((uiBut *)arg)->func_argN);

  uiLayoutSetOperatorContext(layout, lvl->opcontext);
  uiItemsEnumR(layout, &lvl->rnapoin, lvl->propname);
}

void uiItemMenuEnumR_prop(uiLayout *layout,
                          PointerRNA *ptr,
                          PropertyRNA *prop,
                          const std::optional<StringRefNull> name,
                          int icon)
{
  if (layout->root->type == UI_LAYOUT_MENU && !icon) {
    icon = ICON_BLANK1;
  }

  MenuItemLevel *lvl = MEM_new<MenuItemLevel>("MenuItemLevel");
  lvl->rnapoin = *ptr;
  STRNCPY(lvl->propname, RNA_property_identifier(prop));
  lvl->opcontext = layout->root->opcontext;

  ui_item_menu(layout,
               name.value_or(RNA_property_ui_name(prop)),
               icon,
               menu_item_enum_rna_menu,
               nullptr,
               lvl,
               RNA_property_description(prop),
               false,
               but_func_argN_free<MenuItemLevel>,
               but_func_argN_copy<MenuItemLevel>);
}

void uiItemTabsEnumR_prop(uiLayout *layout,
                          bContext *C,
                          PointerRNA *ptr,
                          PropertyRNA *prop,
                          PointerRNA *ptr_highlight,
                          PropertyRNA *prop_highlight,
                          bool icon_only)
{
  uiBlock *block = layout->root->block;

  UI_block_layout_set_current(block, layout);
  ui_item_enum_expand_tabs(layout,
                           C,
                           block,
                           ptr,
                           prop,
                           ptr_highlight,
                           prop_highlight,
                           std::nullopt,
                           UI_UNIT_Y,
                           icon_only);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout Items
 * \{ */

/* single-row layout */
static void ui_litem_estimate_row(uiLayout *litem)
{
  int itemw, itemh;
  bool min_size_flag = true;

  litem->w = 0;
  litem->h = 0;

  if (litem->items.is_empty()) {
    return;
  }

  const uiItem *item_last = litem->items.last();
  for (uiItem *item : litem->items) {
    const bool is_item_last = (item == item_last);
    ui_item_size(item, &itemw, &itemh);

    min_size_flag = min_size_flag && bool(item->flag & uiItemInternalFlag::FixedSize);

    litem->w += itemw;
    litem->h = std::max(itemh, litem->h);

    if (!is_item_last) {
      litem->w += litem->space;
    }
  }

  if (min_size_flag) {
    litem->flag |= uiItemInternalFlag::FixedSize;
  }
}

static int ui_litem_min_width(int itemw)
{
  return std::min(2 * UI_UNIT_X, itemw);
}

static void ui_litem_layout_row(uiLayout *litem)
{
  if (litem->items.is_empty()) {
    return;
  }

  int last_free_item_idx = -1;
  int x, neww, newtotw, itemw, minw, itemh, offset;
  int freew, fixedx, freex, flag = 0, lastw = 0;
  float extra_pixel;

  // x = litem->x; /* UNUSED */
  const int y = litem->y;
  int w = litem->w;
  int totw = 0;
  int tot = 0;

  for (uiItem *item : litem->items) {
    ui_item_size(item, &itemw, &itemh);
    totw += itemw;
    tot++;
  }

  if (totw == 0) {
    return;
  }

  if (w != 0) {
    w -= (tot - 1) * litem->space;
  }
  int fixedw = 0;

  const uiItem *item_last = litem->items.last();

  /* keep clamping items to fixed minimum size until all are done */
  do {
    freew = 0;
    x = 0;
    flag = 0;
    newtotw = totw;
    extra_pixel = 0.0f;

    for (uiItem *item : litem->items) {
      if (bool(item->flag & uiItemInternalFlag::AutoFixedSize)) {
        continue;
      }
      const bool is_item_last = (item == item_last);

      ui_item_size(item, &itemw, &itemh);
      minw = ui_litem_min_width(itemw);

      if (w - lastw > 0) {
        neww = ui_item_fit(
            itemw, x, totw, w - lastw, is_item_last, litem->alignment, &extra_pixel);
      }
      else {
        neww = 0; /* no space left, all will need clamping to minimum size */
      }

      x += neww;

      bool min_flag = bool(item->flag & uiItemInternalFlag::FixedSize);
      /* ignore min flag for rows with right or center alignment */
      if (item->type != uiItemType::Button &&
          ELEM((static_cast<uiLayout *>(item))->alignment,
               UI_LAYOUT_ALIGN_RIGHT,
               UI_LAYOUT_ALIGN_CENTER) &&
          litem->alignment == UI_LAYOUT_ALIGN_EXPAND &&
          bool(litem->flag & uiItemInternalFlag::FixedSize))
      {
        min_flag = false;
      }

      if ((neww < minw || min_flag) && w != 0) {
        /* fixed size */
        item->flag |= uiItemInternalFlag::AutoFixedSize;
        if (item->type != uiItemType::Button && bool(item->flag & uiItemInternalFlag::FixedSize)) {
          minw = itemw;
        }
        fixedw += minw;
        flag = 1;
        newtotw -= itemw;
      }
      else {
        /* keep free size */
        item->flag &= ~uiItemInternalFlag::AutoFixedSize;
        freew += itemw;
      }
    }

    totw = newtotw;
    lastw = fixedw;
  } while (flag);

  freex = 0;
  fixedx = 0;
  extra_pixel = 0.0f;
  x = litem->x;

  int item_idx = -1;
  for (uiItem *item : litem->items) {
    item_idx++;
    const bool is_item_last = (item == item_last);
    ui_item_size(item, &itemw, &itemh);
    minw = ui_litem_min_width(itemw);

    if (bool(item->flag & uiItemInternalFlag::AutoFixedSize)) {
      /* fixed minimum size items */
      if (item->type != uiItemType::Button && bool(item->flag & uiItemInternalFlag::FixedSize)) {
        minw = itemw;
      }
      itemw = ui_item_fit(
          minw, fixedx, fixedw, min_ii(w, fixedw), is_item_last, litem->alignment, &extra_pixel);
      fixedx += itemw;
    }
    else {
      /* free size item */
      itemw = ui_item_fit(
          itemw, freex, freew, w - fixedw, is_item_last, litem->alignment, &extra_pixel);
      freex += itemw;
      last_free_item_idx = item_idx;
    }

    /* align right/center */
    offset = 0;
    if (litem->alignment == UI_LAYOUT_ALIGN_RIGHT) {
      if (freew + fixedw > 0 && freew + fixedw < w) {
        offset = w - (fixedw + freew);
      }
    }
    else if (litem->alignment == UI_LAYOUT_ALIGN_CENTER) {
      if (freew + fixedw > 0 && freew + fixedw < w) {
        offset = (w - (fixedw + freew)) / 2;
      }
    }

    /* position item */
    ui_item_position(item, x + offset, y - itemh, itemw, itemh);

    x += itemw;
    if (!is_item_last) {
      x += litem->space;
    }
  }

  /* add extra pixel */
  int extra_pixel_move = litem->w - (x - litem->x);
  if (extra_pixel_move > 0 && litem->alignment == UI_LAYOUT_ALIGN_EXPAND &&
      last_free_item_idx >= 0 && item_last &&
      bool(item_last->flag & uiItemInternalFlag::AutoFixedSize))
  {
    ui_item_move(litem->items[last_free_item_idx], 0, extra_pixel_move);
    blender::MutableSpan<uiItem *> items_after_last_free =
        litem->items.as_mutable_span().drop_front(last_free_item_idx + 1);
    for (uiItem *item : items_after_last_free) {
      ui_item_move(item, extra_pixel_move, extra_pixel_move);
    }
  }

  litem->w = x - litem->x;
  litem->h = litem->y - y;
  litem->x = x;
  litem->y = y;
}

static int spaces_after_column_item(const uiLayout *litem,
                                    const uiItem *item,
                                    const uiItem *next_item,
                                    const bool is_box)
{
  if (next_item == nullptr) {
    return 0;
  }
  if (item->type == uiItemType::LayoutPanelHeader &&
      next_item->type == uiItemType::LayoutPanelHeader)
  {
    /* No extra space between layout panel headers. */
    return 0;
  }
  if (item->type == uiItemType::LayoutPanelBody &&
      !ELEM(next_item->type, uiItemType::LayoutPanelHeader, uiItemType::LayoutPanelBody))
  {
    /* One for the end of the panel and one at the start of the parent panel. */
    return 2;
  }
  if (!is_box) {
    return 1;
  }
  if (item != litem->items.first()) {
    return 1;
  }
  return 0;
}

/* single-column layout */
static void ui_litem_estimate_column(uiLayout *litem, bool is_box)
{
  int itemw, itemh;
  bool min_size_flag = true;

  litem->w = 0;
  litem->h = 0;

  for (auto *iter = litem->items.begin(); iter != litem->items.end(); iter++) {
    uiItem *item = *iter;
    ui_item_size(item, &itemw, &itemh);

    min_size_flag = min_size_flag && bool(item->flag & uiItemInternalFlag::FixedSize);

    litem->w = std::max(litem->w, itemw);
    litem->h += itemh;

    const uiItem *next_item = (item == litem->items.last()) ? nullptr : *(iter + 1);
    const int spaces_num = spaces_after_column_item(litem, item, next_item, is_box);
    litem->h += spaces_num * litem->space;
  }

  if (min_size_flag) {
    litem->flag |= uiItemInternalFlag::FixedSize;
  }
}

static void ui_litem_layout_column(uiLayout *litem, bool is_box, bool is_menu)
{
  const int x = litem->x;
  int y = litem->y;

  for (auto *iter = litem->items.begin(); iter != litem->items.end(); iter++) {
    uiItem *item = *iter;
    int itemw, itemh;
    ui_item_size(item, &itemw, &itemh);

    y -= itemh;
    ui_item_position(item, x, y, is_menu ? itemw : litem->w, itemh);

    const uiItem *next_item = (item == litem->items.last()) ? nullptr : *(iter + 1);
    const int spaces_num = spaces_after_column_item(litem, item, next_item, is_box);
    y -= spaces_num * litem->space;

    if (is_box) {
      item->flag |= uiItemInternalFlag::BoxItem;
    }
  }

  litem->h = litem->y - y;
  litem->x = x;
  litem->y = y;
}

/* calculates the angle of a specified button in a radial menu,
 * stores a float vector in unit circle */
static RadialDirection ui_get_radialbut_vec(float vec[2], short itemnum)
{
  if (itemnum >= PIE_MAX_ITEMS) {
    itemnum %= PIE_MAX_ITEMS;
    printf("Warning: Pie menus with more than %i items are currently unsupported\n",
           PIE_MAX_ITEMS);
  }

  const RadialDirection dir = RadialDirection(ui_radial_dir_order[itemnum]);
  ui_but_pie_dir(dir, vec);

  return dir;
}

static bool ui_item_is_radial_displayable(uiItem *item)
{

  if ((item->type == uiItemType::Button) &&
      ((static_cast<uiButtonItem *>(item))->but->type == UI_BTYPE_LABEL))
  {
    return false;
  }

  return true;
}

static bool ui_item_is_radial_drawable(uiButtonItem *bitem)
{

  if (ELEM(bitem->but->type, UI_BTYPE_SEPR, UI_BTYPE_SEPR_LINE, UI_BTYPE_SEPR_SPACER)) {
    return false;
  }

  return true;
}

static void ui_litem_layout_radial(uiLayout *litem)
{
  int itemh, itemw;
  int itemnum = 0;

  /* For the radial layout we will use Matt Ebb's design
   * for radiation, see http://mattebb.com/weblog/radiation/
   * also the old code at #5103. */

  const int pie_radius = U.pie_menu_radius * UI_SCALE_FAC;

  const int x = litem->x;
  const int y = litem->y;

  int minx = x, miny = y, maxx = x, maxy = y;

  litem->root->block->pie_data.pie_dir_mask = 0;

  for (uiItem *item : litem->items) {
    /* Not all button types are drawn in a radial menu, do filtering here. */
    if (!ui_item_is_radial_displayable(item)) {
      continue;
    }

    float vec[2];
    const RadialDirection dir = ui_get_radialbut_vec(vec, itemnum);
    const float factor[2] = {
        (vec[0] > 0.01f) ? 0.0f : ((vec[0] < -0.01f) ? -1.0f : -0.5f),
        (vec[1] > 0.99f) ? 0.0f : ((vec[1] < -0.99f) ? -1.0f : -0.5f),
    };
    itemnum++;

    /* Enable for non-buttons because a direction may reference a layout, see: #112610. */
    bool use_dir = true;

    if (item->type == uiItemType::Button) {
      uiButtonItem *bitem = static_cast<uiButtonItem *>(item);

      bitem->but->pie_dir = dir;
      /* Scale the buttons. */
      bitem->but->rect.ymax *= 1.5f;
      /* Add a little bit more here to include number. */
      bitem->but->rect.xmax += 1.5f * UI_UNIT_X;
      /* Enable drawing as pie item if supported by widget. */
      if (ui_item_is_radial_drawable(bitem)) {
        bitem->but->emboss = blender::ui::EmbossType::PieMenu;
        bitem->but->drawflag |= UI_BUT_ICON_LEFT;
      }

      if (ELEM(bitem->but->type, UI_BTYPE_SEPR, UI_BTYPE_SEPR_LINE)) {
        use_dir = false;
      }
    }

    if (use_dir) {
      litem->root->block->pie_data.pie_dir_mask |= 1 << int(dir);
    }

    ui_item_size(item, &itemw, &itemh);

    ui_item_position(item,
                     x + (vec[0] * pie_radius) + (factor[0] * itemw),
                     y + (vec[1] * pie_radius) + (factor[1] * itemh),
                     itemw,
                     itemh);

    minx = min_ii(minx, x + (vec[0] * pie_radius) - (itemw / 2));
    maxx = max_ii(maxx, x + (vec[0] * pie_radius) + (itemw / 2));
    miny = min_ii(miny, y + (vec[1] * pie_radius) - (itemh / 2));
    maxy = max_ii(maxy, y + (vec[1] * pie_radius) + (itemh / 2));
  }

  litem->x = minx;
  litem->y = miny;
  litem->w = maxx - minx;
  litem->h = maxy - miny;
}

/* root layout */
static void ui_litem_estimate_root(uiLayout * /*litem*/)
{
  /* nothing to do */
}

static void ui_litem_layout_root_radial(uiLayout *litem)
{
  /* first item is pie menu title, align on center of menu */
  uiItem *item = litem->items.first();

  if (item->type == uiItemType::Button) {
    int itemh, itemw, x, y;
    x = litem->x;
    y = litem->y;

    ui_item_size(item, &itemw, &itemh);

    ui_item_position(
        item, x - itemw / 2, y + UI_SCALE_FAC * (U.pie_menu_threshold + 9.0f), itemw, itemh);
  }
}

static void ui_litem_layout_root(uiLayout *litem)
{
  if (litem->root->type == UI_LAYOUT_HEADER) {
    ui_litem_layout_row(litem);
  }
  else if (litem->root->type == UI_LAYOUT_PIEMENU) {
    ui_litem_layout_root_radial(litem);
  }
  else if (litem->root->type == UI_LAYOUT_MENU) {
    ui_litem_layout_column(litem, false, true);
  }
  else {
    ui_litem_layout_column(litem, false, false);
  }
}

/* panel header layout */
static void ui_litem_estimate_panel_header(uiLayout *litem)
{
  BLI_assert(litem->items.size() == 1);
  uiItem *item = litem->items.first();

  int w, h;
  ui_item_size(item, &w, &h);
  litem->w = w;
  litem->h = h;
}

static void ui_litem_layout_panel_header(uiLayout *litem)
{
  uiLayoutItemPanelHeader *header_litem = static_cast<uiLayoutItemPanelHeader *>(litem);
  Panel *panel = litem->root->block->panel;

  BLI_assert(litem->items.size() == 1);
  uiItem *item = litem->items.first();

  int w, h;
  ui_item_size(item, &w, &h);
  litem->y -= h;
  ui_item_position(item, litem->x, litem->y, litem->w, h);
  const float offset = UI_style_get_dpi()->panelspace;
  panel->runtime->layout_panels.headers.append({float(litem->y) - offset,
                                                float(litem->y + litem->h) - offset,
                                                header_litem->open_prop_owner,
                                                header_litem->open_prop_name});
}

/* panel body layout */
static void ui_litem_estimate_panel_body(uiLayout *litem)
{
  ui_litem_estimate_column(litem, false);
}

static void ui_litem_layout_panel_body(uiLayout *litem)
{
  Panel *panel = litem->root->block->panel;
  ui_litem_layout_column(litem, false, false);
  const float offset = UI_style_get_dpi()->panelspace;
  panel->runtime->layout_panels.bodies.append({
      float(litem->y - litem->space) - offset,
      float(litem->y + litem->h + litem->space) - offset,
  });
}

/* box layout */
static void ui_litem_estimate_box(uiLayout *litem)
{
  const uiStyle *style = litem->root->style;

  ui_litem_estimate_column(litem, true);

  int boxspace = style->boxspace;
  if (litem->root->type == UI_LAYOUT_HEADER) {
    boxspace = 0;
  }
  litem->w += 2 * boxspace;
  litem->h += 2 * boxspace;
}

static void ui_litem_layout_box(uiLayout *litem)
{
  uiLayoutItemBx *box = static_cast<uiLayoutItemBx *>(litem);
  const uiStyle *style = litem->root->style;

  int boxspace = style->boxspace;
  if (litem->root->type == UI_LAYOUT_HEADER) {
    boxspace = 0;
  }

  const int w = litem->w;
  const int h = litem->h;

  litem->x += boxspace;
  litem->y -= boxspace;

  if (w != 0) {
    litem->w -= 2 * boxspace;
  }
  if (h != 0) {
    litem->h -= 2 * boxspace;
  }

  ui_litem_layout_column(litem, true, false);

  litem->x -= boxspace;
  litem->y -= boxspace;

  if (w != 0) {
    litem->w += 2 * boxspace;
  }
  if (h != 0) {
    litem->h += 2 * boxspace;
  }

  /* roundbox around the sublayout */
  uiBut *but = box->roundbox;
  but->rect.xmin = litem->x;
  but->rect.ymin = litem->y;
  but->rect.xmax = litem->x + litem->w;
  but->rect.ymax = litem->y + litem->h;
}

/* multi-column layout, automatically flowing to the next */
static void ui_litem_estimate_column_flow(uiLayout *litem)
{
  const uiStyle *style = litem->root->style;
  uiLayoutItemFlow *flow = static_cast<uiLayoutItemFlow *>(litem);
  int itemw, itemh, maxw = 0;

  /* compute max needed width and total height */
  int toth = 0;
  int totitem = 0;
  for (uiItem *item : litem->items) {
    ui_item_size(item, &itemw, &itemh);
    maxw = std::max(maxw, itemw);
    toth += itemh;
    totitem++;
  }

  if (flow->number <= 0) {
    /* auto compute number of columns, not very good */
    if (maxw == 0) {
      flow->totcol = 1;
      return;
    }

    flow->totcol = max_ii(litem->root->emw / maxw, 1);
    flow->totcol = min_ii(flow->totcol, totitem);
  }
  else {
    flow->totcol = flow->number;
  }

  /* compute sizes */
  int x = 0;
  int y = 0;
  int emy = 0;
  int miny = 0;

  maxw = 0;
  const int emh = toth / flow->totcol;

  /* create column per column */
  int col = 0;
  for (uiItem *item : litem->items) {
    ui_item_size(item, &itemw, &itemh);

    y -= itemh + style->buttonspacey;
    miny = min_ii(miny, y);
    emy -= itemh;
    maxw = max_ii(itemw, maxw);

    /* decide to go to next one */
    if (col < flow->totcol - 1 && emy <= -emh) {
      x += maxw + litem->space;
      maxw = 0;
      y = 0;
      emy = 0; /* need to reset height again for next column */
      col++;
    }
  }

  litem->w = x;
  litem->h = litem->y - miny;
}

static void ui_litem_layout_column_flow(uiLayout *litem)
{
  const uiStyle *style = litem->root->style;
  uiLayoutItemFlow *flow = static_cast<uiLayoutItemFlow *>(litem);
  int col, emh, itemw, itemh;

  /* compute max needed width and total height */
  int toth = 0;
  for (uiItem *item : litem->items) {
    ui_item_size(item, &itemw, &itemh);
    toth += itemh;
  }

  /* compute sizes */
  int x = litem->x;
  int y = litem->y;
  int emy = 0;
  int miny = 0;

  emh = toth / flow->totcol;

  /* create column per column */
  col = 0;
  int w = (litem->w - (flow->totcol - 1) * style->columnspace) / flow->totcol;
  for (uiItem *item : litem->items) {
    ui_item_size(item, &itemw, &itemh);

    itemw = (litem->alignment == UI_LAYOUT_ALIGN_EXPAND) ? w : min_ii(w, itemw);

    y -= itemh;
    emy -= itemh;
    ui_item_position(item, x, y, itemw, itemh);
    y -= style->buttonspacey;
    miny = min_ii(miny, y);

    /* decide to go to next one */
    if (col < flow->totcol - 1 && emy <= -emh) {
      x += w + style->columnspace;
      y = litem->y;
      emy = 0; /* need to reset height again for next column */
      col++;

      const int remaining_width = litem->w - (x - litem->x);
      const int remaining_width_between_columns = (flow->totcol - col - 1) * style->columnspace;
      const int remaining_columns = flow->totcol - col;
      w = (remaining_width - remaining_width_between_columns) / remaining_columns;
    }
  }

  litem->h = litem->y - miny;
  litem->x = x;
  litem->y = miny;
}

/* multi-column and multi-row layout. */
struct UILayoutGridFlowInput {
  /* General layout control settings. */
  bool row_major : 1;    /* Fill rows before columns */
  bool even_columns : 1; /* All columns will have same width. */
  bool even_rows : 1;    /* All rows will have same height. */
  int space_x;           /* Space between columns. */
  int space_y;           /* Space between rows. */
                         /* Real data about current position and size of this layout item
                          * (either estimated, or final values). */
  int litem_w;           /* Layout item width. */
  int litem_x;           /* Layout item X position. */
  int litem_y;           /* Layout item Y position. */
  /* Actual number of columns and rows to generate (computed from first pass usually). */
  int tot_columns; /* Number of columns. */
  int tot_rows;    /* Number of rows. */
};

struct UILayoutGridFlowOutput {
  int *tot_items; /* Total number of items in this grid layout. */
  /* Width / X pos data. */
  float *global_avg_w; /* Computed average width of the columns. */
  int *cos_x_array;    /* Computed X coordinate of each column. */
  int *widths_array;   /* Computed width of each column. */
  int *tot_w;          /* Computed total width. */
  /* Height / Y pos data. */
  int *global_max_h;  /* Computed height of the tallest item in the grid. */
  int *cos_y_array;   /* Computed Y coordinate of each column. */
  int *heights_array; /* Computed height of each column. */
  int *tot_h;         /* Computed total height. */
};

static void ui_litem_grid_flow_compute(blender::Span<uiItem *> items,
                                       const UILayoutGridFlowInput *parameters,
                                       UILayoutGridFlowOutput *results)
{
  float tot_w = 0.0f, tot_h = 0.0f;
  float global_avg_w = 0.0f, global_totweight_w = 0.0f;
  int global_max_h = 0;

  BLI_assert(parameters->tot_columns != 0 ||
             (results->cos_x_array == nullptr && results->widths_array == nullptr &&
              results->tot_w == nullptr));
  BLI_assert(parameters->tot_rows != 0 ||
             (results->cos_y_array == nullptr && results->heights_array == nullptr &&
              results->tot_h == nullptr));

  if (results->tot_items) {
    *results->tot_items = 0;
  }

  if (items.is_empty()) {
    if (results->global_avg_w) {
      *results->global_avg_w = 0.0f;
    }
    if (results->global_max_h) {
      *results->global_max_h = 0;
    }
    return;
  }

  blender::Array<float, 64> avg_w(parameters->tot_columns, 0.0f);
  blender::Array<float, 64> totweight_w(parameters->tot_columns, 0.0f);
  blender::Array<int, 64> max_h(parameters->tot_rows, 0);

  int i = 0;
  for (const uiItem *item : items) {
    int item_w, item_h;
    ui_item_size(item, &item_w, &item_h);

    global_avg_w += float(item_w * item_w);
    global_totweight_w += float(item_w);
    global_max_h = max_ii(global_max_h, item_h);

    if (parameters->tot_rows != 0 && parameters->tot_columns != 0) {
      const int index_col = parameters->row_major ? i % parameters->tot_columns :
                                                    i / parameters->tot_rows;
      const int index_row = parameters->row_major ? i / parameters->tot_columns :
                                                    i % parameters->tot_rows;

      avg_w[index_col] += float(item_w * item_w);
      totweight_w[index_col] += float(item_w);

      max_h[index_row] = max_ii(max_h[index_row], item_h);
    }

    if (results->tot_items) {
      (*results->tot_items)++;
    }
    i++;
  }

  /* Finalize computing of column average sizes */
  global_avg_w /= global_totweight_w;
  if (parameters->tot_columns != 0) {
    for (i = 0; i < parameters->tot_columns; i++) {
      avg_w[i] /= totweight_w[i];
      tot_w += avg_w[i];
    }
    if (parameters->even_columns) {
      tot_w = ceilf(global_avg_w) * parameters->tot_columns;
    }
  }
  /* Finalize computing of rows max sizes */
  if (parameters->tot_rows != 0) {
    for (i = 0; i < parameters->tot_rows; i++) {
      tot_h += max_h[i];
    }
    if (parameters->even_rows) {
      tot_h = global_max_h * parameters->tot_columns;
    }
  }

  /* Compute positions and sizes of all cells. */
  if (results->cos_x_array != nullptr && results->widths_array != nullptr) {
    /* We enlarge/narrow columns evenly to match available width. */
    const float wfac = float(parameters->litem_w -
                             (parameters->tot_columns - 1) * parameters->space_x) /
                       tot_w;

    for (int col = 0; col < parameters->tot_columns; col++) {
      results->cos_x_array[col] = (col ? results->cos_x_array[col - 1] +
                                             results->widths_array[col - 1] + parameters->space_x :
                                         parameters->litem_x);
      if (parameters->even_columns) {
        /* (< remaining width > - < space between remaining columns >) / < remaining columns > */
        results->widths_array[col] = (((parameters->litem_w -
                                        (results->cos_x_array[col] - parameters->litem_x)) -
                                       (parameters->tot_columns - col - 1) * parameters->space_x) /
                                      (parameters->tot_columns - col));
      }
      else if (col == parameters->tot_columns - 1) {
        /* Last column copes width rounding errors... */
        results->widths_array[col] = parameters->litem_w -
                                     (results->cos_x_array[col] - parameters->litem_x);
      }
      else {
        results->widths_array[col] = int(avg_w[col] * wfac);
      }
    }
  }
  if (results->cos_y_array != nullptr && results->heights_array != nullptr) {
    for (int row = 0; row < parameters->tot_rows; row++) {
      if (parameters->even_rows) {
        results->heights_array[row] = global_max_h;
      }
      else {
        results->heights_array[row] = max_h[row];
      }
      results->cos_y_array[row] = (row ? results->cos_y_array[row - 1] - parameters->space_y -
                                             results->heights_array[row] :
                                         parameters->litem_y - results->heights_array[row]);
    }
  }

  if (results->global_avg_w) {
    *results->global_avg_w = global_avg_w;
  }
  if (results->global_max_h) {
    *results->global_max_h = global_max_h;
  }
  if (results->tot_w) {
    *results->tot_w = int(tot_w) + parameters->space_x * (parameters->tot_columns - 1);
  }
  if (results->tot_h) {
    *results->tot_h = tot_h + parameters->space_y * (parameters->tot_rows - 1);
  }
}

static void ui_litem_estimate_grid_flow(uiLayout *litem)
{
  const uiStyle *style = litem->root->style;
  uiLayoutItemGridFlow *gflow = static_cast<uiLayoutItemGridFlow *>(litem);

  const int space_x = style->columnspace;
  const int space_y = style->buttonspacey;

  /* Estimate average needed width and height per item. */
  {
    float avg_w;
    int max_h;

    UILayoutGridFlowInput input{};
    input.row_major = gflow->row_major;
    input.even_columns = gflow->even_columns;
    input.even_rows = gflow->even_rows;
    input.litem_w = litem->w;
    input.litem_x = litem->x;
    input.litem_y = litem->y;
    input.space_x = space_x;
    input.space_y = space_y;
    UILayoutGridFlowOutput output{};
    output.tot_items = &gflow->tot_items;
    output.global_avg_w = &avg_w;
    output.global_max_h = &max_h;
    ui_litem_grid_flow_compute(litem->items, &input, &output);

    if (gflow->tot_items == 0) {
      litem->w = litem->h = 0;
      gflow->tot_columns = gflow->tot_rows = 0;
      return;
    }

    /* Even in varying column width case,
     * we fix our columns number from weighted average width of items,
     * a proper solving of required width would be too costly,
     * and this should give reasonably good results in all reasonable cases. */
    if (gflow->columns_len > 0) {
      gflow->tot_columns = gflow->columns_len;
    }
    else {
      if (avg_w == 0.0f) {
        gflow->tot_columns = 1;
      }
      else {
        gflow->tot_columns = min_ii(max_ii(int(litem->w / avg_w), 1), gflow->tot_items);
      }
    }
    gflow->tot_rows = int(ceilf(float(gflow->tot_items) / gflow->tot_columns));

    /* Try to tweak number of columns and rows to get better filling of last column or row,
     * and apply 'modulo' value to number of columns or rows.
     * Note that modulo does not prevent ending with fewer columns/rows than modulo, if mandatory
     * to avoid empty column/row. */
    {
      const int modulo = (gflow->columns_len < -1) ? -gflow->columns_len : 0;
      const int step = modulo ? modulo : 1;

      if (gflow->row_major) {
        /* Adjust number of columns to be multiple of given modulo. */
        if (modulo && gflow->tot_columns % modulo != 0 && gflow->tot_columns > modulo) {
          gflow->tot_columns = gflow->tot_columns - (gflow->tot_columns % modulo);
        }
        /* Find smallest number of columns conserving computed optimal number of rows. */
        for (gflow->tot_rows = int(ceilf(float(gflow->tot_items) / gflow->tot_columns));
             (gflow->tot_columns - step) > 0 &&
             int(ceilf(float(gflow->tot_items) / (gflow->tot_columns - step))) <= gflow->tot_rows;
             gflow->tot_columns -= step)
        {
          /* pass */
        }
      }
      else {
        /* Adjust number of rows to be multiple of given modulo. */
        if (modulo && gflow->tot_rows % modulo != 0) {
          gflow->tot_rows = min_ii(gflow->tot_rows + modulo - (gflow->tot_rows % modulo),
                                   gflow->tot_items);
        }
        /* Find smallest number of rows conserving computed optimal number of columns. */
        for (gflow->tot_columns = int(ceilf(float(gflow->tot_items) / gflow->tot_rows));
             (gflow->tot_rows - step) > 0 &&
             int(ceilf(float(gflow->tot_items) / (gflow->tot_rows - step))) <= gflow->tot_columns;
             gflow->tot_rows -= step)
        {
          /* pass */
        }
      }
    }

    /* Set evenly-spaced axes size
     * (quick optimization in case we have even columns and rows). */
    if (gflow->even_columns && gflow->even_rows) {
      litem->w = int(gflow->tot_columns * avg_w) + space_x * (gflow->tot_columns - 1);
      litem->h = int(gflow->tot_rows * max_h) + space_y * (gflow->tot_rows - 1);
      return;
    }
  }

  /* Now that we have our final number of columns and rows,
   * we can compute actual needed space for non-evenly sized axes. */
  {
    int tot_w, tot_h;
    UILayoutGridFlowInput input{};
    input.row_major = gflow->row_major;
    input.even_columns = gflow->even_columns;
    input.even_rows = gflow->even_rows;
    input.litem_w = litem->w;
    input.litem_x = litem->x;
    input.litem_y = litem->y;
    input.space_x = space_x;
    input.space_y = space_y;
    input.tot_columns = gflow->tot_columns;
    input.tot_rows = gflow->tot_rows;
    UILayoutGridFlowOutput output{};
    output.tot_w = &tot_w;
    output.tot_h = &tot_h;
    ui_litem_grid_flow_compute(litem->items, &input, &output);

    litem->w = tot_w;
    litem->h = tot_h;
  }
}

static void ui_litem_layout_grid_flow(uiLayout *litem)
{
  const uiStyle *style = litem->root->style;
  uiLayoutItemGridFlow *gflow = static_cast<uiLayoutItemGridFlow *>(litem);

  if (gflow->tot_items == 0) {
    litem->w = litem->h = 0;
    return;
  }

  BLI_assert(gflow->tot_columns > 0);
  BLI_assert(gflow->tot_rows > 0);

  const int space_x = style->columnspace;
  const int space_y = style->buttonspacey;

  blender::Array<int, 64> widths(gflow->tot_columns);
  blender::Array<int, 64> heights(gflow->tot_rows);
  blender::Array<int, 64> cos_x(gflow->tot_columns);
  blender::Array<int, 64> cos_y(gflow->tot_rows);

  /* This time we directly compute coordinates and sizes of all cells. */
  UILayoutGridFlowInput input{};
  input.row_major = gflow->row_major;
  input.even_columns = gflow->even_columns;
  input.even_rows = gflow->even_rows;
  input.litem_w = litem->w;
  input.litem_x = litem->x;
  input.litem_y = litem->y;
  input.space_x = space_x;
  input.space_y = space_y;
  input.tot_columns = gflow->tot_columns;
  input.tot_rows = gflow->tot_rows;
  UILayoutGridFlowOutput output{};
  output.cos_x_array = cos_x.data();
  output.cos_y_array = cos_y.data();
  output.widths_array = widths.data();
  output.heights_array = heights.data();
  ui_litem_grid_flow_compute(litem->items, &input, &output);

  int i = 0;
  for (uiItem *item : litem->items) {
    const int col = gflow->row_major ? i % gflow->tot_columns : i / gflow->tot_rows;
    const int row = gflow->row_major ? i / gflow->tot_columns : i % gflow->tot_rows;
    int item_w, item_h;
    ui_item_size(item, &item_w, &item_h);

    const int w = widths[col];
    const int h = heights[row];

    item_w = (litem->alignment == UI_LAYOUT_ALIGN_EXPAND) ? w : min_ii(w, item_w);
    item_h = (litem->alignment == UI_LAYOUT_ALIGN_EXPAND) ? h : min_ii(h, item_h);

    ui_item_position(item, cos_x[col], cos_y[row], item_w, item_h);
    i++;
  }

  litem->h = litem->y - cos_y[gflow->tot_rows - 1];
  litem->x = (cos_x[gflow->tot_columns - 1] - litem->x) + widths[gflow->tot_columns - 1];
  litem->y = litem->y - litem->h;
}

/* free layout */
static void ui_litem_estimate_absolute(uiLayout *litem)
{
  int minx = 1e6;
  int miny = 1e6;
  litem->w = 0;
  litem->h = 0;

  for (uiItem *item : litem->items) {
    int itemx, itemy, itemw, itemh;
    ui_item_offset(item, &itemx, &itemy);
    ui_item_size(item, &itemw, &itemh);

    minx = min_ii(minx, itemx);
    miny = min_ii(miny, itemy);

    litem->w = std::max(litem->w, itemx + itemw);
    litem->h = std::max(litem->h, itemy + itemh);
  }

  litem->w -= minx;
  litem->h -= miny;
}

static void ui_litem_layout_absolute(uiLayout *litem)
{
  float scalex = 1.0f, scaley = 1.0f;
  int x, y, newx, newy, itemx, itemy, itemh, itemw;

  int minx = 1e6;
  int miny = 1e6;
  int totw = 0;
  int toth = 0;

  for (uiItem *item : litem->items) {
    ui_item_offset(item, &itemx, &itemy);
    ui_item_size(item, &itemw, &itemh);

    minx = min_ii(minx, itemx);
    miny = min_ii(miny, itemy);

    totw = max_ii(totw, itemx + itemw);
    toth = max_ii(toth, itemy + itemh);
  }

  totw -= minx;
  toth -= miny;

  if (litem->w && totw > 0) {
    scalex = float(litem->w) / float(totw);
  }
  if (litem->h && toth > 0) {
    scaley = float(litem->h) / float(toth);
  }

  x = litem->x;
  y = litem->y - scaley * toth;

  for (uiItem *item : litem->items) {
    ui_item_offset(item, &itemx, &itemy);
    ui_item_size(item, &itemw, &itemh);

    if (scalex != 1.0f) {
      newx = (itemx - minx) * scalex;
      itemw = (itemx - minx + itemw) * scalex - newx;
      itemx = minx + newx;
    }

    if (scaley != 1.0f) {
      newy = (itemy - miny) * scaley;
      itemh = (itemy - miny + itemh) * scaley - newy;
      itemy = miny + newy;
    }

    ui_item_position(item, x + itemx - minx, y + itemy - miny, itemw, itemh);
  }

  litem->w = scalex * totw;
  litem->h = litem->y - y;
  litem->x = x + litem->w;
  litem->y = y;
}

/* split layout */
static void ui_litem_estimate_split(uiLayout *litem)
{
  ui_litem_estimate_row(litem);
  litem->flag &= ~uiItemInternalFlag::FixedSize;
}

static void ui_litem_layout_split(uiLayout *litem)
{
  uiLayoutItemSplit *split = static_cast<uiLayoutItemSplit *>(litem);
  float extra_pixel = 0.0f;
  const int tot = int(litem->items.size());

  if (tot == 0) {
    return;
  }

  int x = litem->x;
  const int y = litem->y;

  const float percentage = (split->percentage == 0.0f) ? 1.0f / float(tot) : split->percentage;

  const int w = (litem->w - (tot - 1) * litem->space);
  int colw = w * percentage;
  colw = std::max(colw, 0);

  const uiItem *item_last = litem->items.last();
  for (uiItem *item : litem->items) {
    const bool is_item_last = (item == item_last);
    int itemh;
    ui_item_size(item, nullptr, &itemh);

    ui_item_position(item, x, y - itemh, colw, itemh);
    x += colw;

    if (!is_item_last) {
      const float width = extra_pixel + (w - int(w * percentage)) / (float(tot) - 1);
      extra_pixel = width - int(width);
      colw = int(width);
      colw = std::max(colw, 0);

      x += litem->space;
    }
  }

  litem->w = x - litem->x;
  litem->h = litem->y - y;
  litem->x = x;
  litem->y = y;
}

/* overlap layout */
static void ui_litem_estimate_overlap(uiLayout *litem)
{
  litem->w = 0;
  litem->h = 0;

  for (uiItem *item : litem->items) {
    int itemw, itemh;
    ui_item_size(item, &itemw, &itemh);

    litem->w = std::max(itemw, litem->w);
    litem->h = std::max(itemh, litem->h);
  }
}

static void ui_litem_layout_overlap(uiLayout *litem)
{

  const int x = litem->x;
  const int y = litem->y;

  for (uiItem *item : litem->items) {
    int itemw, itemh;
    ui_item_size(item, &itemw, &itemh);
    ui_item_position(item, x, y - itemh, litem->w, itemh);

    litem->h = std::max(litem->h, itemh);
  }

  litem->x = x;
  litem->y = y - litem->h;
}

static void ui_litem_init_from_parent(uiLayout *litem, uiLayout *layout, int align)
{
  litem->root = layout->root;
  litem->align = align;
  /* Children of grid-flow layout shall never have "ideal big size" returned as estimated size. */
  litem->variable_size = layout->variable_size || layout->type == uiItemType::LayoutGridFlow;
  litem->active = true;
  litem->enabled = true;
  litem->context = layout->context;
  litem->redalert = layout->redalert;
  litem->w = layout->w;
  litem->emboss = layout->emboss;
  litem->flag = (layout->flag & (uiItemInternalFlag::PropSep | uiItemInternalFlag::PropDecorate |
                                 uiItemInternalFlag::InsidePropSep));

  if (layout->child_items_layout) {
    layout->child_items_layout->items.append(litem);
    litem->parent = layout->child_items_layout;
  }
  else {
    layout->items.append(litem);
    litem->parent = layout;
  }
}

static void ui_layout_heading_set(uiLayout *layout, const StringRef heading)
{
  heading.copy_utf8_truncated(layout->heading);
}

uiLayout *uiLayoutRow(uiLayout *layout, bool align)
{
  uiLayout *litem = MEM_new<uiLayout>(__func__);
  ui_litem_init_from_parent(litem, layout, align);

  litem->type = uiItemType::LayoutRow;
  litem->space = (align) ? 0 : layout->root->style->buttonspacex;

  UI_block_layout_set_current(layout->root->block, litem);

  return litem;
}

PanelLayout uiLayoutPanelProp(const bContext *C,
                              uiLayout *layout,
                              PointerRNA *open_prop_owner,
                              const StringRefNull open_prop_name)
{
  const ARegion *region = CTX_wm_region(C);

  const bool is_real_open = RNA_boolean_get(open_prop_owner, open_prop_name.c_str());
  const bool search_filter_active = region->flag & RGN_FLAG_SEARCH_FILTER_ACTIVE;
  const bool is_open = is_real_open || search_filter_active;

  PanelLayout panel_layout{};
  {
    uiLayoutItemPanelHeader *header_litem = MEM_new<uiLayoutItemPanelHeader>(__func__);
    ui_litem_init_from_parent(header_litem, layout, false);
    header_litem->type = uiItemType::LayoutPanelHeader;

    header_litem->open_prop_owner = *open_prop_owner;
    STRNCPY(header_litem->open_prop_name, open_prop_name.c_str());

    uiLayout *row = uiLayoutRow(header_litem, true);
    uiLayoutSetUnitsY(row, 1.2f);

    uiBlock *block = uiLayoutGetBlock(row);
    const int icon = is_open ? ICON_DOWNARROW_HLT : ICON_RIGHTARROW;
    const int width = ui_text_icon_width(layout, "", icon, false);
    uiDefIconTextBut(
        block, UI_BTYPE_LABEL, 0, icon, "", 0, 0, width, UI_UNIT_Y, nullptr, 0.0f, 0.0f, "");

    panel_layout.header = row;
  }

  if (!is_open) {
    return panel_layout;
  }

  uiLayoutItemPanelBody *body_litem = MEM_new<uiLayoutItemPanelBody>(__func__);
  body_litem->type = uiItemType::LayoutPanelBody;
  body_litem->space = layout->root->style->templatespace;
  ui_litem_init_from_parent(body_litem, layout, false);
  UI_block_layout_set_current(layout->root->block, body_litem);
  panel_layout.body = body_litem;

  return panel_layout;
}

PanelLayout uiLayoutPanelPropWithBoolHeader(const bContext *C,
                                            uiLayout *layout,
                                            PointerRNA *open_prop_owner,
                                            const StringRefNull open_prop_name,
                                            PointerRNA *bool_prop_owner,
                                            const StringRefNull bool_prop_name,
                                            const std::optional<StringRefNull> label)
{
  PanelLayout panel = uiLayoutPanelProp(C, layout, open_prop_owner, open_prop_name);

  uiLayout *panel_header = panel.header;
  panel_header->flag &= ~(uiItemInternalFlag::PropSep | uiItemInternalFlag::PropDecorate |
                          uiItemInternalFlag::InsidePropSep);
  uiItemR(panel_header, bool_prop_owner, bool_prop_name, UI_ITEM_NONE, label, ICON_NONE);

  return panel;
}

uiLayout *uiLayoutPanelProp(const bContext *C,
                            uiLayout *layout,
                            PointerRNA *open_prop_owner,
                            const StringRefNull open_prop_name,
                            const StringRef label)
{
  PanelLayout panel = uiLayoutPanelProp(C, layout, open_prop_owner, open_prop_name);
  uiItemL(panel.header, label, ICON_NONE);

  return panel.body;
}

PanelLayout uiLayoutPanel(const bContext *C,
                          uiLayout *layout,
                          const StringRef idname,
                          const bool default_closed)
{
  Panel *panel = uiLayoutGetRootPanel(layout);
  BLI_assert(panel != nullptr);

  LayoutPanelState *state = BKE_panel_layout_panel_state_ensure(panel, idname, default_closed);
  PointerRNA state_ptr = RNA_pointer_create_discrete(nullptr, &RNA_LayoutPanelState, state);

  return uiLayoutPanelProp(C, layout, &state_ptr, "is_open");
}

uiLayout *uiLayoutPanel(const bContext *C,
                        uiLayout *layout,
                        const StringRef idname,
                        const bool default_closed,
                        const StringRef label)
{
  PanelLayout panel = uiLayoutPanel(C, layout, idname, default_closed);
  uiItemL(panel.header, label, ICON_NONE);

  return panel.body;
}

bool uiLayoutEndsWithPanelHeader(const uiLayout &layout)
{
  if (layout.items.is_empty()) {
    return false;
  }
  const uiItem *item = layout.items.last();
  return item->type == uiItemType::LayoutPanelHeader;
}

uiLayout *uiLayoutRowWithHeading(uiLayout *layout, bool align, const StringRef heading)
{
  uiLayout *litem = uiLayoutRow(layout, align);
  ui_layout_heading_set(litem, heading);
  return litem;
}

uiLayout *uiLayoutColumn(uiLayout *layout, bool align)
{
  uiLayout *litem = MEM_new<uiLayout>(__func__);
  ui_litem_init_from_parent(litem, layout, align);

  litem->type = uiItemType::LayoutColumn;
  litem->space = (align) ? 0 : layout->root->style->buttonspacey;

  UI_block_layout_set_current(layout->root->block, litem);

  return litem;
}

uiLayout *uiLayoutColumnWithHeading(uiLayout *layout, bool align, const StringRef heading)
{
  uiLayout *litem = uiLayoutColumn(layout, align);
  ui_layout_heading_set(litem, heading);
  return litem;
}

uiLayout *uiLayoutColumnFlow(uiLayout *layout, int number, bool align)
{
  uiLayoutItemFlow *flow = MEM_new<uiLayoutItemFlow>(__func__);
  ui_litem_init_from_parent(flow, layout, align);

  flow->type = uiItemType::LayoutColumnFlow;
  flow->space = (flow->align) ? 0 : layout->root->style->columnspace;
  flow->number = number;

  UI_block_layout_set_current(layout->root->block, flow);

  return flow;
}

uiLayout *uiLayoutGridFlow(uiLayout *layout,
                           bool row_major,
                           int columns_len,
                           bool even_columns,
                           bool even_rows,
                           bool align)
{
  uiLayoutItemGridFlow *flow = MEM_new<uiLayoutItemGridFlow>(__func__);
  flow->type = uiItemType::LayoutGridFlow;
  ui_litem_init_from_parent(flow, layout, align);

  flow->space = (flow->align) ? 0 : layout->root->style->columnspace;
  flow->row_major = row_major;
  flow->columns_len = columns_len;
  flow->even_columns = even_columns;
  flow->even_rows = even_rows;

  UI_block_layout_set_current(layout->root->block, flow);

  return flow;
}

static uiLayoutItemBx *ui_layout_box(uiLayout *layout, int type)
{
  uiLayoutItemBx *box = MEM_new<uiLayoutItemBx>(__func__);
  ui_litem_init_from_parent(box, layout, false);

  box->type = uiItemType::LayoutBox;
  box->space = layout->root->style->columnspace;

  UI_block_layout_set_current(layout->root->block, box);

  box->roundbox = uiDefBut(layout->root->block, type, 0, "", 0, 0, 0, 0, nullptr, 0.0, 0.0, "");

  return box;
}

uiLayout *uiLayoutRadial(uiLayout *layout)
{
  /* radial layouts are only valid for radial menus */
  if (layout->root->type != UI_LAYOUT_PIEMENU) {
    return ui_item_local_sublayout(layout, layout, false);
  }

  /* only one radial wheel per root layout is allowed, so check and return that, if it exists */
  for (uiItem *item : layout->root->layout->items) {
    if (item->type == uiItemType::LayoutRadial) {
      uiLayout *litem = static_cast<uiLayout *>(item);
      UI_block_layout_set_current(layout->root->block, litem);
      return litem;
    }
  }

  uiLayout *litem = MEM_new<uiLayout>(__func__);
  ui_litem_init_from_parent(litem, layout, false);

  litem->type = uiItemType::LayoutRadial;

  UI_block_layout_set_current(layout->root->block, litem);

  return litem;
}

uiLayout *uiLayoutBox(uiLayout *layout)
{
  return (uiLayout *)ui_layout_box(layout, UI_BTYPE_ROUNDBOX);
}

void ui_layout_list_set_labels_active(uiLayout *layout)
{
  for (uiItem *item : layout->items) {
    if (item->type != uiItemType::Button) {
      ui_layout_list_set_labels_active(static_cast<uiLayout *>(item));
    }
    else {
      uiButtonItem *bitem = static_cast<uiButtonItem *>(item);
      if (bitem->but->flag & UI_BUT_LIST_ITEM) {
        UI_but_flag_enable(bitem->but, UI_SELECT);
      }
    }
  }
}

uiLayout *uiLayoutListBox(uiLayout *layout,
                          uiList *ui_list,
                          PointerRNA *actptr,
                          PropertyRNA *actprop)
{
  uiLayoutItemBx *box = ui_layout_box(layout, UI_BTYPE_LISTBOX);
  uiBut *but = box->roundbox;

  but->custom_data = ui_list;

  but->rnapoin = *actptr;
  but->rnaprop = actprop;

  /* only for the undo string */
  if (but->flag & UI_BUT_UNDO) {
    but->tip = RNA_property_description(actprop);
  }

  return (uiLayout *)box;
}

uiLayout *uiLayoutAbsolute(uiLayout *layout, bool align)
{
  uiLayout *litem = MEM_new<uiLayout>(__func__);
  ui_litem_init_from_parent(litem, layout, align);

  litem->type = uiItemType::LayoutAbsolute;

  UI_block_layout_set_current(layout->root->block, litem);

  return litem;
}

uiBlock *uiLayoutAbsoluteBlock(uiLayout *layout)
{
  uiBlock *block = uiLayoutGetBlock(layout);
  uiLayoutAbsolute(layout, false);

  return block;
}

uiLayout *uiLayoutOverlap(uiLayout *layout)
{
  uiLayout *litem = MEM_new<uiLayout>(__func__);
  ui_litem_init_from_parent(litem, layout, false);

  litem->type = uiItemType::LayoutOverlap;

  UI_block_layout_set_current(layout->root->block, litem);

  return litem;
}

uiLayout *uiLayoutSplit(uiLayout *layout, float percentage, bool align)
{
  uiLayoutItemSplit *split = MEM_new<uiLayoutItemSplit>(__func__);
  ui_litem_init_from_parent(split, layout, align);

  split->type = uiItemType::LayoutSplit;
  split->space = layout->root->style->columnspace;
  split->percentage = percentage;

  UI_block_layout_set_current(layout->root->block, split);

  return split;
}

void uiLayoutSetActive(uiLayout *layout, bool active)
{
  layout->active = active;
}

void uiLayoutSetActiveDefault(uiLayout *layout, bool active_default)
{
  layout->active_default = active_default;
}

void uiLayoutSetActivateInit(uiLayout *layout, bool activate_init)
{
  layout->activate_init = activate_init;
}

void uiLayoutSetEnabled(uiLayout *layout, bool enabled)
{
  layout->enabled = enabled;
}

void uiLayoutSetRedAlert(uiLayout *layout, bool redalert)
{
  layout->redalert = redalert;
}

void uiLayoutSetKeepAspect(uiLayout *layout, bool keepaspect)
{
  layout->keepaspect = keepaspect;
}

void uiLayoutSetAlignment(uiLayout *layout, char alignment)
{
  layout->alignment = alignment;
}

void uiLayoutSetScaleX(uiLayout *layout, float scale)
{
  layout->scale[0] = scale;
}

void uiLayoutSetScaleY(uiLayout *layout, float scale)
{
  layout->scale[1] = scale;
}

void uiLayoutSetUnitsX(uiLayout *layout, float unit)
{
  layout->units[0] = unit;
}

void uiLayoutSetUnitsY(uiLayout *layout, float unit)
{
  layout->units[1] = unit;
}

void uiLayoutSetEmboss(uiLayout *layout, blender::ui::EmbossType emboss)
{
  layout->emboss = emboss;
}

bool uiLayoutGetPropSep(uiLayout *layout)
{
  return bool(layout->flag & uiItemInternalFlag::PropSep);
}

void uiLayoutSetPropSep(uiLayout *layout, bool is_sep)
{
  SET_FLAG_FROM_TEST(layout->flag, is_sep, uiItemInternalFlag::PropSep);
}

bool uiLayoutGetPropDecorate(uiLayout *layout)
{
  return bool(layout->flag & uiItemInternalFlag::PropDecorate);
}

void uiLayoutSetPropDecorate(uiLayout *layout, bool is_sep)
{
  SET_FLAG_FROM_TEST(layout->flag, is_sep, uiItemInternalFlag::PropDecorate);
}

void uiLayoutSetSearchWeight(uiLayout *layout, const float weight)
{
  layout->search_weight = weight;
}

float uiLayoutGetSearchWeight(uiLayout *layout)
{
  return layout->search_weight;
}

Panel *uiLayoutGetRootPanel(uiLayout *layout)
{
  return layout->root->block->panel;
}

bool uiLayoutGetActive(uiLayout *layout)
{
  return layout->active;
}

bool uiLayoutGetActiveDefault(uiLayout *layout)
{
  return layout->active_default;
}

bool uiLayoutGetActivateInit(uiLayout *layout)
{
  return layout->activate_init;
}

bool uiLayoutGetEnabled(uiLayout *layout)
{
  return layout->enabled;
}

bool uiLayoutGetRedAlert(uiLayout *layout)
{
  return layout->redalert;
}

bool uiLayoutGetKeepAspect(uiLayout *layout)
{
  return layout->keepaspect;
}

int uiLayoutGetAlignment(uiLayout *layout)
{
  return layout->alignment;
}

int uiLayoutGetWidth(uiLayout *layout)
{
  return layout->w;
}

float uiLayoutGetScaleX(uiLayout *layout)
{
  return layout->scale[0];
}

float uiLayoutGetScaleY(uiLayout *layout)
{
  return layout->scale[1];
}

float uiLayoutGetUnitsX(uiLayout *layout)
{
  return layout->units[0];
}

float uiLayoutGetUnitsY(uiLayout *layout)
{
  return layout->units[1];
}

blender::ui::EmbossType uiLayoutGetEmboss(uiLayout *layout)
{
  if (layout->emboss == blender::ui::EmbossType::Undefined) {
    return layout->root->block->emboss;
  }
  return layout->emboss;
}

int uiLayoutListItemPaddingWidth()
{
  return 5 * UI_SCALE_FAC;
}

void uiLayoutListItemAddPadding(uiLayout *layout)
{
  uiBlock *block = uiLayoutGetBlock(layout);
  uiLayout *row = uiLayoutRow(layout, true);
  uiLayoutSetFixedSize(row, true);

  uiDefBut(
      block, UI_BTYPE_SEPR, 0, "", 0, 0, uiLayoutListItemPaddingWidth(), 0, nullptr, 0.0, 0.0, "");

  /* Restore. */
  UI_block_layout_set_current(block, layout);
}

LayoutSuppressFlag uiLayoutSuppressFlagGet(const uiLayout *layout)
{
  return layout->suppress_flag;
}

void uiLayoutSuppressFlagSet(uiLayout *layout, LayoutSuppressFlag flag)
{
  layout->suppress_flag |= flag;
}

void uiLayoutSuppressFlagClear(uiLayout *layout, LayoutSuppressFlag flag)
{
  layout->suppress_flag &= ~flag;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Block Layout Search Filtering
 * \{ */

/* Disabled for performance reasons, but this could be turned on in the future. */
// #define PROPERTY_SEARCH_USE_TOOLTIPS

static bool block_search_panel_label_matches(const uiBlock *block, const char *search_string)
{
  if ((block->panel != nullptr) && (block->panel->type != nullptr)) {
    if (BLI_strcasestr(block->panel->type->label, search_string)) {
      return true;
    }
  }
  return false;
}

/**
 * Returns true if a button or the data / operator it represents matches the search filter.
 */
static bool button_matches_search_filter(uiBut *but, const char *search_filter)
{
  /* Do the shorter checks first for better performance in case there is a match. */
  if (BLI_strcasestr(but->str.c_str(), search_filter)) {
    return true;
  }

  if (but->optype != nullptr) {
    if (BLI_strcasestr(but->optype->name, search_filter)) {
      return true;
    }
  }

  if (but->rnaprop != nullptr) {
    if (BLI_strcasestr(RNA_property_ui_name(but->rnaprop), search_filter)) {
      return true;
    }
#ifdef PROPERTY_SEARCH_USE_TOOLTIPS
    if (BLI_strcasestr(RNA_property_description(but->rnaprop), search_filter)) {
      return true;
    }
#endif

    /* Search through labels of enum property items if they are in a drop-down menu.
     * Unfortunately we have no #bContext here so we cannot search through RNA enums
     * with dynamic entries (or "itemf" functions) which require context. */
    if (but->type == UI_BTYPE_MENU) {
      PointerRNA *ptr = &but->rnapoin;
      PropertyRNA *enum_prop = but->rnaprop;
      int items_len;
      const EnumPropertyItem *items_array = nullptr;
      bool free;
      RNA_property_enum_items_gettexted(nullptr, ptr, enum_prop, &items_array, &items_len, &free);
      if (items_array == nullptr) {
        return false;
      }

      bool found = false;
      for (int i = 0; i < items_len; i++) {
        /* Check for nullptr name field which enums use for separators. */
        if (items_array[i].name == nullptr) {
          continue;
        }
        if (BLI_strcasestr(items_array[i].name, search_filter)) {
          found = true;
          break;
        }
      }
      if (free) {
        MEM_freeN((EnumPropertyItem *)items_array);
      }
      if (found) {
        return true;
      }
    }
  }

  return false;
}

/**
 * Test for a search result within a specific button group.
 */
static bool button_group_has_search_match(const uiButtonGroup &group, const char *search_filter)
{
  for (uiBut *but : group.buttons) {
    if (button_matches_search_filter(but, search_filter)) {
      return true;
    }
  }

  return false;
}

/**
 * Apply the search filter, tagging all buttons with whether they match or not.
 * Tag every button in the group as a result if any button in the group matches.
 *
 * \note It would be great to return early here if we found a match, but because
 * the results may be visible we have to continue searching the entire block.
 *
 * \return True if the block has any search results.
 */
static bool block_search_filter_tag_buttons(uiBlock *block, const char *search_filter)
{
  bool has_result = false;
  for (const uiButtonGroup &group : block->button_groups) {
    if (button_group_has_search_match(group, search_filter)) {
      has_result = true;
    }
    else {
      for (uiBut *but : group.buttons) {
        but->flag |= UI_SEARCH_FILTER_NO_MATCH;
      }
    }
  }
  return has_result;
}

bool UI_block_apply_search_filter(uiBlock *block, const char *search_filter)
{
  if (search_filter == nullptr || search_filter[0] == '\0') {
    return false;
  }

  Panel *panel = block->panel;

  if (panel != nullptr) {
    /* Panels for active blocks should always have a valid `panel->type`,
     * otherwise they wouldn't be created. */
    if (panel->type->flag & PANEL_TYPE_NO_SEARCH) {
      return false;
    }
  }

  const bool panel_label_matches = block_search_panel_label_matches(block, search_filter);

  const bool has_result = (panel_label_matches) ?
                              true :
                              block_search_filter_tag_buttons(block, search_filter);

  if (panel != nullptr) {
    if (has_result) {
      ui_panel_tag_search_filter_match(block->panel);
    }
  }

  return has_result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout
 * \{ */

static void ui_item_scale(uiLayout *litem, const float scale[2])
{
  int x, y, w, h;

  for (auto riter = litem->items.rbegin(); riter != litem->items.rend(); riter++) {
    uiItem *item = *riter;
    if (item->type != uiItemType::Button) {
      uiLayout *subitem = static_cast<uiLayout *>(item);
      ui_item_scale(subitem, scale);
    }

    ui_item_size(item, &w, &h);
    ui_item_offset(item, &x, &y);

    if (scale[0] != 0.0f) {
      x *= scale[0];
      w *= scale[0];
    }

    if (scale[1] != 0.0f) {
      y *= scale[1];
      h *= scale[1];
    }

    ui_item_position(item, x, y, w, h);
  }
}

static void ui_item_estimate(uiItem *item)
{
  if (item->type != uiItemType::Button) {
    uiLayout *litem = static_cast<uiLayout *>(item);

    if (litem->items.is_empty()) {
      litem->w = 0;
      litem->h = 0;
      return;
    }

    for (uiItem *subitem : litem->items) {
      ui_item_estimate(subitem);
    }

    if (litem->scale[0] != 0.0f || litem->scale[1] != 0.0f) {
      ui_item_scale(litem, litem->scale);
    }

    switch (litem->type) {
      case uiItemType::LayoutColumn:
        ui_litem_estimate_column(litem, false);
        break;
      case uiItemType::LayoutColumnFlow:
        ui_litem_estimate_column_flow(litem);
        break;
      case uiItemType::LayoutGridFlow:
        ui_litem_estimate_grid_flow(litem);
        break;
      case uiItemType::LayoutRow:
        ui_litem_estimate_row(litem);
        break;
      case uiItemType::LayoutPanelHeader:
        ui_litem_estimate_panel_header(litem);
        break;
      case uiItemType::LayoutPanelBody:
        ui_litem_estimate_panel_body(litem);
        break;
      case uiItemType::LayoutBox:
        ui_litem_estimate_box(litem);
        break;
      case uiItemType::LayoutRoot:
        ui_litem_estimate_root(litem);
        break;
      case uiItemType::LayoutAbsolute:
        ui_litem_estimate_absolute(litem);
        break;
      case uiItemType::LayoutSplit:
        ui_litem_estimate_split(litem);
        break;
      case uiItemType::LayoutOverlap:
        ui_litem_estimate_overlap(litem);
        break;
      default:
        break;
    }

    /* Force fixed size. */
    if (litem->units[0] > 0) {
      litem->w = UI_UNIT_X * litem->units[0];
    }
    if (litem->units[1] > 0) {
      litem->h = UI_UNIT_Y * litem->units[1];
    }
  }
}

static void ui_item_align(uiLayout *litem, short nr)
{
  for (auto riter = litem->items.rbegin(); riter != litem->items.rend(); riter++) {
    uiItem *item = *riter;
    if (item->type == uiItemType::Button) {
      uiButtonItem *bitem = static_cast<uiButtonItem *>(item);
#ifndef USE_UIBUT_SPATIAL_ALIGN
      if (ui_but_can_align(bitem->but))
#endif
      {
        if (!bitem->but->alignnr) {
          bitem->but->alignnr = nr;
        }
      }
    }
    else if (item->type == uiItemType::LayoutAbsolute) {
      /* pass */
    }
    else if (item->type == uiItemType::LayoutOverlap) {
      /* pass */
    }
    else if (item->type == uiItemType::LayoutBox) {
      uiLayoutItemBx *box = static_cast<uiLayoutItemBx *>(item);
      if (!box->roundbox->alignnr) {
        box->roundbox->alignnr = nr;
      }
    }
    else {
      uiLayout *litem = static_cast<uiLayout *>(item);
      if (litem->align) {
        ui_item_align(litem, nr);
      }
    }
  }
}

static void ui_item_flag(uiLayout *litem, int flag)
{
  for (auto riter = litem->items.rbegin(); riter != litem->items.rend(); riter++) {
    uiItem *item = *riter;
    if (item->type == uiItemType::Button) {
      uiButtonItem *bitem = static_cast<uiButtonItem *>(item);
      bitem->but->flag |= flag;
    }
    else {
      ui_item_flag(static_cast<uiLayout *>(item), flag);
    }
  }
}

static void ui_item_layout(uiItem *item)
{
  if (item->type != uiItemType::Button) {
    uiLayout *litem = static_cast<uiLayout *>(item);

    if (litem->items.is_empty()) {
      return;
    }

    if (litem->align) {
      ui_item_align(litem, ++litem->root->block->alignnr);
    }
    if (!litem->active) {
      ui_item_flag(litem, UI_BUT_INACTIVE);
    }
    if (!litem->enabled) {
      ui_item_flag(litem, UI_BUT_DISABLED);
    }

    switch (litem->type) {
      case uiItemType::LayoutColumn:
        ui_litem_layout_column(litem, false, false);
        break;
      case uiItemType::LayoutColumnFlow:
        ui_litem_layout_column_flow(litem);
        break;
      case uiItemType::LayoutGridFlow:
        ui_litem_layout_grid_flow(litem);
        break;
      case uiItemType::LayoutRow:
        ui_litem_layout_row(litem);
        break;
      case uiItemType::LayoutPanelHeader:
        ui_litem_layout_panel_header(litem);
        break;
      case uiItemType::LayoutPanelBody:
        ui_litem_layout_panel_body(litem);
        break;
      case uiItemType::LayoutBox:
        ui_litem_layout_box(litem);
        break;
      case uiItemType::LayoutRoot:
        ui_litem_layout_root(litem);
        break;
      case uiItemType::LayoutAbsolute:
        ui_litem_layout_absolute(litem);
        break;
      case uiItemType::LayoutSplit:
        ui_litem_layout_split(litem);
        break;
      case uiItemType::LayoutOverlap:
        ui_litem_layout_overlap(litem);
        break;
      case uiItemType::LayoutRadial:
        ui_litem_layout_radial(litem);
        break;
      default:
        break;
    }

    for (uiItem *subitem : litem->items) {
      if (bool(item->flag & uiItemInternalFlag::BoxItem)) {
        subitem->flag |= uiItemInternalFlag::BoxItem;
      }
      ui_item_layout(subitem);
    }
  }
  else {
    if (bool(item->flag & uiItemInternalFlag::BoxItem)) {
      uiButtonItem *bitem = static_cast<uiButtonItem *>(item);
      bitem->but->drawflag |= UI_BUT_BOX_ITEM;
    }
  }
}

static void ui_layout_end(uiBlock *block, uiLayout *layout, int *r_x, int *r_y)
{
  if (layout->root->handlefunc) {
    UI_block_func_handle_set(block, layout->root->handlefunc, layout->root->argv);
  }

  ui_item_estimate(layout);
  ui_item_layout(layout);

  if (r_x) {
    *r_x = layout->x;
  }
  if (r_y) {
    *r_y = layout->y;
  }
}

static void ui_layout_free(uiLayout *layout)
{
  for (uiItem *item : layout->items) {
    if (item->type == uiItemType::Button) {
      uiButtonItem *bitem = static_cast<uiButtonItem *>(item);

      bitem->but->layout = nullptr;
      MEM_delete(item);
    }
    else {
      uiLayout *litem = static_cast<uiLayout *>(item);
      ui_layout_free(litem);
    }
  }

  MEM_delete(layout);
}

static void ui_layout_add_padding_button(uiLayoutRoot *root)
{
  if (root->padding) {
    /* add an invisible button for padding */
    uiBlock *block = root->block;
    uiLayout *prev_layout = block->curlayout;

    block->curlayout = root->layout;
    uiDefBut(
        block, UI_BTYPE_SEPR, 0, "", 0, 0, root->padding, root->padding, nullptr, 0.0, 0.0, "");
    block->curlayout = prev_layout;
  }
}

uiLayout *UI_block_layout(uiBlock *block,
                          int dir,
                          int type,
                          int x,
                          int y,
                          int size,
                          int em,
                          int padding,
                          const uiStyle *style)
{
  uiLayoutRoot *root = MEM_callocN<uiLayoutRoot>(__func__);
  root->type = type;
  root->style = style;
  root->block = block;
  root->padding = padding;
  root->opcontext = WM_OP_INVOKE_REGION_WIN;

  uiLayout *layout = MEM_new<uiLayout>(__func__);
  layout->type = (type == UI_LAYOUT_VERT_BAR) ? uiItemType::LayoutColumn : uiItemType::LayoutRoot;

  /* Only used when 'uiItemInternalFlag::PropSep' is set. */
  layout->flag = uiItemInternalFlag::PropDecorate;

  layout->x = x;
  layout->y = y;
  layout->root = root;
  layout->space = style->templatespace;
  layout->active = true;
  layout->enabled = true;
  layout->context = nullptr;
  layout->emboss = blender::ui::EmbossType::Undefined;

  if (ELEM(type, UI_LAYOUT_MENU, UI_LAYOUT_PIEMENU)) {
    layout->space = 0;
  }

  if (dir == UI_LAYOUT_HORIZONTAL) {
    layout->h = size;
    layout->root->emh = em * UI_UNIT_Y;
  }
  else {
    layout->w = size;
    layout->root->emw = em * UI_UNIT_X;
  }

  block->curlayout = layout;
  root->layout = layout;
  BLI_addtail(&block->layouts, root);

  ui_layout_add_padding_button(root);

  return layout;
}

uiBlock *uiLayoutGetBlock(uiLayout *layout)
{
  return layout->root->block;
}

wmOperatorCallContext uiLayoutGetOperatorContext(uiLayout *layout)
{
  return layout->root->opcontext;
}

void UI_block_layout_set_current(uiBlock *block, uiLayout *layout)
{
  block->curlayout = layout;
}

void ui_layout_add_but(uiLayout *layout, uiBut *but)
{
  uiButtonItem *bitem = MEM_new<uiButtonItem>(__func__);
  bitem->type = uiItemType::Button;
  bitem->but = but;

  int w, h;
  ui_item_size((uiItem *)bitem, &w, &h);
  /* XXX uiBut hasn't scaled yet
   * we can flag the button as not expandable, depending on its size */
  if (w <= 2 * UI_UNIT_X && but->str.empty()) {
    bitem->flag |= uiItemInternalFlag::FixedSize;
  }

  if (layout->child_items_layout) {
    layout->child_items_layout->items.append(bitem);
  }
  else {
    layout->items.append(bitem);
  }
  but->layout = layout;
  but->search_weight = layout->search_weight;

  if (layout->context) {
    but->context = layout->context;
    layout->context->used = true;
  }

  if (layout->emboss != blender::ui::EmbossType::Undefined) {
    but->emboss = layout->emboss;
  }

  ui_button_group_add_but(uiLayoutGetBlock(layout), but);
}

static uiButtonItem *ui_layout_find_button_item(const uiLayout *layout, const uiBut *but)
{
  const blender::Vector<uiItem *> &child_list = layout->child_items_layout ?
                                                    layout->child_items_layout->items :
                                                    layout->items;

  for (uiItem *item : child_list) {
    if (item->type == uiItemType::Button) {
      uiButtonItem *bitem = static_cast<uiButtonItem *>(item);

      if (bitem->but == but) {
        return bitem;
      }
    }
    else {
      uiButtonItem *nested_item = ui_layout_find_button_item(static_cast<uiLayout *>(item), but);
      if (nested_item) {
        return nested_item;
      }
    }
  }

  return nullptr;
}

void ui_layout_remove_but(uiLayout *layout, const uiBut *but)
{
  blender::Vector<uiItem *> &child_list = layout->child_items_layout ?
                                              layout->child_items_layout->items :
                                              layout->items;
  const int64_t removed_num = child_list.remove_if([but](auto item) {
    if (item->type == uiItemType::Button) {
      uiButtonItem *bitem = static_cast<uiButtonItem *>(item);
      return (bitem->but == but);
    }
    return false;
  });

  BLI_assert(removed_num <= 1);
  UNUSED_VARS_NDEBUG(removed_num);
}

bool ui_layout_replace_but_ptr(uiLayout *layout, const void *old_but_ptr, uiBut *new_but)
{
  uiButtonItem *bitem = ui_layout_find_button_item(layout,
                                                   static_cast<const uiBut *>(old_but_ptr));
  if (!bitem) {
    return false;
  }

  bitem->but = new_but;
  return true;
}

void uiLayoutSetFixedSize(uiLayout *layout, bool fixed_size)
{
  if (fixed_size) {
    layout->flag |= uiItemInternalFlag::FixedSize;
  }
  else {
    layout->flag &= ~uiItemInternalFlag::FixedSize;
  }
}

bool uiLayoutGetFixedSize(uiLayout *layout)
{
  return bool(layout->flag & uiItemInternalFlag::FixedSize);
}

void uiLayoutSetOperatorContext(uiLayout *layout, wmOperatorCallContext opcontext)
{
  layout->root->opcontext = opcontext;
}

void uiLayoutSetFunc(uiLayout *layout, uiMenuHandleFunc handlefunc, void *argv)
{
  layout->root->handlefunc = handlefunc;
  layout->root->argv = argv;
}

void UI_block_layout_free(uiBlock *block)
{
  LISTBASE_FOREACH_MUTABLE (uiLayoutRoot *, root, &block->layouts) {
    ui_layout_free(root->layout);
    MEM_freeN(root);
  }
}

void UI_block_layout_resolve(uiBlock *block, int *r_x, int *r_y)
{
  BLI_assert(block->active);

  if (r_x) {
    *r_x = 0;
  }
  if (r_y) {
    *r_y = 0;
  }

  block->curlayout = nullptr;

  LISTBASE_FOREACH_MUTABLE (uiLayoutRoot *, root, &block->layouts) {
    ui_layout_add_padding_button(root);

    /* nullptr in advance so we don't interfere when adding button */
    ui_layout_end(block, root->layout, r_x, r_y);
    ui_layout_free(root->layout);
    MEM_freeN(root);
  }

  BLI_listbase_clear(&block->layouts);
}

bool UI_block_layout_needs_resolving(const uiBlock *block)
{
  return !BLI_listbase_is_empty(&block->layouts);
}

void uiLayoutSetContextPointer(uiLayout *layout, StringRef name, PointerRNA *ptr)
{
  uiBlock *block = layout->root->block;
  layout->context = CTX_store_add(block->contexts, name, ptr);
}

void uiLayoutSetContextString(uiLayout *layout, StringRef name, blender::StringRef value)
{
  uiBlock *block = layout->root->block;
  layout->context = CTX_store_add(block->contexts, name, value);
}

void uiLayoutSetContextInt(uiLayout *layout, StringRef name, int64_t value)
{
  uiBlock *block = layout->root->block;
  layout->context = CTX_store_add(block->contexts, name, value);
}

bContextStore *uiLayoutGetContextStore(uiLayout *layout)
{
  return layout->context;
}

void uiLayoutContextCopy(uiLayout *layout, const bContextStore *context)
{
  uiBlock *block = layout->root->block;
  layout->context = CTX_store_add_all(block->contexts, context);
}

void uiLayoutSetTooltipFunc(uiLayout *layout,
                            uiButToolTipFunc func,
                            void *arg,
                            uiCopyArgFunc copy_arg,
                            uiFreeArgFunc free_arg)
{
  bool arg_used = false;

  for (uiItem *item : layout->items) {
    /* Each button will call free_arg for "its" argument, so we need to
     * duplicate the allocation for each button after the first. */
    if (copy_arg != nullptr && arg_used) {
      arg = copy_arg(arg);
    }

    if (item->type == uiItemType::Button) {
      uiButtonItem *bitem = static_cast<uiButtonItem *>(item);
      if (bitem->but->type == UI_BTYPE_DECORATOR) {
        continue;
      }
      UI_but_func_tooltip_set(bitem->but, func, arg, free_arg);
      arg_used = true;
    }
    else {
      uiLayoutSetTooltipFunc(static_cast<uiLayout *>(item), func, arg, copy_arg, free_arg);
      arg_used = true;
    }
  }

  if (free_arg != nullptr && !arg_used) {
    /* Free the original copy of arg in case the layout is empty. */
    free_arg(arg);
  }
}

void uiLayoutSetContextFromBut(uiLayout *layout, uiBut *but)
{
  if (but->opptr) {
    uiLayoutSetContextPointer(layout, "button_operator", but->opptr);
  }

  if (but->rnapoin.data && but->rnaprop) {
    /* TODO: index could be supported as well */
    PointerRNA ptr_prop = RNA_pointer_create_discrete(nullptr, &RNA_Property, but->rnaprop);
    uiLayoutSetContextPointer(layout, "button_prop", &ptr_prop);
    uiLayoutSetContextPointer(layout, "button_pointer", &but->rnapoin);
  }
}

wmOperatorType *UI_but_operatortype_get_from_enum_menu(uiBut *but, PropertyRNA **r_prop)
{
  if (r_prop != nullptr) {
    *r_prop = nullptr;
  }

  if (but->menu_create_func == menu_item_enum_opname_menu) {
    MenuItemLevel *lvl = static_cast<MenuItemLevel *>(but->func_argN);
    wmOperatorType *ot = WM_operatortype_find(lvl->opname, false);
    if ((ot != nullptr) && (r_prop != nullptr)) {
      *r_prop = RNA_struct_type_find_property(ot->srna, lvl->propname);
    }
    return ot;
  }
  return nullptr;
}

MenuType *UI_but_menutype_get(const uiBut *but)
{
  if (but->menu_create_func == ui_item_menutype_func) {
    return (MenuType *)but->poin;
  }
  return nullptr;
}

PanelType *UI_but_paneltype_get(const uiBut *but)
{
  if (but->menu_create_func == ui_item_paneltype_func) {
    return (PanelType *)but->poin;
  }
  return nullptr;
}

std::optional<blender::StringRefNull> UI_but_asset_shelf_type_idname_get(const uiBut *but)
{
  return UI_asset_shelf_idname_from_button_context(but);
}

void UI_menutype_draw(bContext *C, MenuType *mt, uiLayout *layout)
{
  Menu menu{};
  menu.layout = layout;
  menu.type = mt;

  if (G.debug & G_DEBUG_WM) {
    printf("%s: opening menu \"%s\"\n", __func__, mt->idname);
  }

  uiBlock *block = uiLayoutGetBlock(layout);
  if (bool(mt->flag & MenuTypeFlag::SearchOnKeyPress)) {
    UI_block_flag_enable(block, UI_BLOCK_NO_ACCELERATOR_KEYS);
  }
  if (mt->listener) {
    /* Forward the menu type listener to the block we're drawing in. */
    ui_block_add_dynamic_listener(block, mt->listener);
  }

  if (layout->context) {
    CTX_store_set(C, layout->context);
  }

  mt->draw(C, &menu);

  if (layout->context) {
    CTX_store_set(C, nullptr);
  }
}

static bool ui_layout_has_panel_label(const uiLayout *layout, const PanelType *pt)
{
  for (uiItem *subitem : layout->items) {
    if (subitem->type == uiItemType::Button) {
      uiButtonItem *bitem = static_cast<uiButtonItem *>(subitem);
      if (!(bitem->but->flag & UI_HIDDEN) &&
          bitem->but->str == CTX_IFACE_(pt->translation_context, pt->label))
      {
        return true;
      }
    }
    else {
      uiLayout *litem = static_cast<uiLayout *>(subitem);
      if (ui_layout_has_panel_label(litem, pt)) {
        return true;
      }
    }
  }

  return false;
}

static void ui_paneltype_draw_impl(bContext *C, PanelType *pt, uiLayout *layout, bool show_header)
{
  uiBlock *block = uiLayoutGetBlock(layout);
  Panel *panel = BKE_panel_new(pt);
  panel->flag = PNL_POPOVER;

  if (pt->listener) {
    ui_block_add_dynamic_listener(block, pt->listener);
  }

  /* This check may be paranoid, this function might run outside the context of a popup or can run
   * in popovers that are not supposed to support refreshing, see #ui_popover_create_block. */
  if (block->handle && block->handle->region) {
    /* Allow popovers to contain collapsible sections, see #uiItemPopoverPanel. */
    UI_popup_dummy_panel_set(block->handle->region, block);
  }

  uiItem *item_last = layout->items.is_empty() ? nullptr : layout->items.last();

  /* Draw main panel. */
  if (show_header) {
    uiLayout *row = uiLayoutRow(layout, false);
    if (pt->draw_header) {
      panel->layout = row;
      pt->draw_header(C, panel);
      panel->layout = nullptr;
    }

    /* draw_header() is often used to add a checkbox to the header. If we add the label like below
     * the label is disconnected from the checkbox, adding a weird looking gap. As workaround, let
     * the checkbox add the label instead. */
    if (!ui_layout_has_panel_label(row, pt)) {
      uiItemL(row, CTX_IFACE_(pt->translation_context, pt->label), ICON_NONE);
    }
  }

  panel->layout = layout;
  pt->draw(C, panel);
  panel->layout = nullptr;
  BLI_assert(panel->runtime->custom_data_ptr == nullptr);

  BKE_panel_free(panel);

  /* Draw child panels. */
  LISTBASE_FOREACH (LinkData *, link, &pt->children) {
    PanelType *child_pt = static_cast<PanelType *>(link->data);

    if (child_pt->poll == nullptr || child_pt->poll(C, child_pt)) {
      /* Add space if something was added to the layout. */
      if (!layout->items.is_empty() && item_last != layout->items.last()) {
        uiItemS(layout);
        item_last = layout->items.last();
      }

      uiLayout *col = uiLayoutColumn(layout, false);
      ui_paneltype_draw_impl(C, child_pt, col, true);
    }
  }
}

void UI_paneltype_draw(bContext *C, PanelType *pt, uiLayout *layout)
{
  if (layout->context) {
    CTX_store_set(C, layout->context);
  }

  ui_paneltype_draw_impl(C, pt, layout, false);

  if (layout->context) {
    CTX_store_set(C, nullptr);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout (Debugging/Introspection)
 *
 * Serialize the layout as a Python compatible dictionary,
 *
 * \note Proper string escaping isn't used,
 * triple quotes are used to prevent single quotes from interfering with Python syntax.
 * If we want this to be fool-proof, we would need full Python compatible string escape support.
 * As we don't use triple quotes in the UI it's good-enough in practice.
 * \{ */

static void ui_layout_introspect_button(DynStr *ds, const uiButtonItem *bitem)
{
  uiBut *but = bitem->but;
  BLI_dynstr_appendf(ds, "'type':%d, ", int(but->type));
  BLI_dynstr_appendf(ds, "'draw_string':'''%s''', ", but->drawstr.c_str());
  /* Not exactly needed, rna has this. */
  BLI_dynstr_appendf(ds, "'tip':'''%s''', ", std::string(but->tip).c_str());

  if (but->optype) {
    std::string opstr = WM_operator_pystring_ex(static_cast<bContext *>(but->block->evil_C),
                                                nullptr,
                                                false,
                                                true,
                                                but->optype,
                                                but->opptr);
    BLI_dynstr_appendf(ds, "'operator':'''%s''', ", opstr.c_str());
  }

  {
    PropertyRNA *prop = nullptr;
    wmOperatorType *ot = UI_but_operatortype_get_from_enum_menu(but, &prop);
    if (ot) {
      std::string opstr = WM_operator_pystring_ex(
          static_cast<bContext *>(but->block->evil_C), nullptr, false, true, ot, nullptr);
      BLI_dynstr_appendf(ds, "'operator':'''%s''', ", opstr.c_str());
      BLI_dynstr_appendf(ds, "'property':'''%s''', ", prop ? RNA_property_identifier(prop) : "");
    }
  }

  if (but->rnaprop) {
    BLI_dynstr_appendf(ds,
                       "'rna':'%s.%s[%d]', ",
                       RNA_struct_identifier(but->rnapoin.type),
                       RNA_property_identifier(but->rnaprop),
                       but->rnaindex);
  }
}

static void ui_layout_introspect_items(DynStr *ds, blender::Span<uiItem *> items)
{
  BLI_dynstr_append(ds, "[");

  for (const uiItem *item : items) {

    BLI_dynstr_append(ds, "{");

#define CASE_ITEM(type, name) \
  case type: { \
    BLI_dynstr_append(ds, "'type': '"); \
    BLI_dynstr_append(ds, name); \
    BLI_dynstr_append(ds, "', "); \
    break; \
  } \
    ((void)0)

    switch (item->type) {
      CASE_ITEM(uiItemType::Button, "BUTTON");
      CASE_ITEM(uiItemType::LayoutRow, "LAYOUT_ROW");
      CASE_ITEM(uiItemType::LayoutPanelHeader, "LAYOUT_PANEL_HEADER");
      CASE_ITEM(uiItemType::LayoutPanelBody, "LAYOUT_PANEL_BODY");
      CASE_ITEM(uiItemType::LayoutColumn, "LAYOUT_COLUMN");
      CASE_ITEM(uiItemType::LayoutColumnFlow, "LAYOUT_COLUMN_FLOW");
      CASE_ITEM(uiItemType::LayoutRowFlow, "LAYOUT_ROW_FLOW");
      CASE_ITEM(uiItemType::LayoutBox, "LAYOUT_BOX");
      CASE_ITEM(uiItemType::LayoutAbsolute, "LAYOUT_ABSOLUTE");
      CASE_ITEM(uiItemType::LayoutSplit, "LAYOUT_SPLIT");
      CASE_ITEM(uiItemType::LayoutOverlap, "LAYOUT_OVERLAP");
      CASE_ITEM(uiItemType::LayoutRoot, "LAYOUT_ROOT");
      CASE_ITEM(uiItemType::LayoutGridFlow, "LAYOUT_GRID_FLOW");
      CASE_ITEM(uiItemType::LayoutRadial, "LAYOUT_RADIAL");
    }

#undef CASE_ITEM

    switch (item->type) {
      case uiItemType::Button:
        ui_layout_introspect_button(ds, static_cast<const uiButtonItem *>(item));
        break;
      default:
        BLI_dynstr_append(ds, "'items':");
        ui_layout_introspect_items(ds, (static_cast<const uiLayout *>(item))->items);
        break;
    }

    BLI_dynstr_append(ds, "}");

    if (item != items.last()) {
      BLI_dynstr_append(ds, ", ");
    }
  }
  /* Don't use a comma here as it's not needed and
   * causes the result to evaluate to a tuple of 1. */
  BLI_dynstr_append(ds, "]");
}

const char *UI_layout_introspect(uiLayout *layout)
{
  DynStr *ds = BLI_dynstr_new();
  uiLayout layout_copy(*layout);
  blender::Vector<uiItem *> layout_dummy_list(1, static_cast<uiItem *>(&layout_copy));
  ui_layout_introspect_items(ds, layout_dummy_list);
  const char *result = BLI_dynstr_get_cstring(ds);
  BLI_dynstr_free(ds);
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Alert Box with Big Icon
 * \{ */

uiLayout *uiItemsAlertBox(uiBlock *block,
                          const uiStyle *style,
                          const int dialog_width,
                          const eAlertIcon icon,
                          const int icon_size)
{
  /* By default, the space between icon and text/buttons will be equal to the 'columnspace',
   * this extra padding will add some space by increasing the left column width,
   * making the icon placement more symmetrical, between the block edge and the text. */
  const float icon_padding = 5.0f * UI_SCALE_FAC;
  /* Calculate the factor of the fixed icon column depending on the block width. */
  const float split_factor = (float(icon_size) + icon_padding) /
                             float(dialog_width - style->columnspace);

  uiLayout *block_layout = UI_block_layout(
      block, UI_LAYOUT_VERTICAL, UI_LAYOUT_PANEL, 0, 0, dialog_width, 0, 0, style);

  /* Split layout to put alert icon on left side. */
  uiLayout *split_block = uiLayoutSplit(block_layout, split_factor, false);

  /* Alert icon on the left. */
  uiLayout *layout = uiLayoutRow(split_block, false);
  /* Using 'align_left' with 'row' avoids stretching the icon along the width of column. */
  uiLayoutSetAlignment(layout, UI_LAYOUT_ALIGN_LEFT);
  uiDefButAlert(block, icon, 0, 0, icon_size, icon_size);

  /* The rest of the content on the right. */
  layout = uiLayoutColumn(split_block, false);

  return layout;
}

uiLayout *uiItemsAlertBox(uiBlock *block, const int size, const eAlertIcon icon)
{
  const uiStyle *style = UI_style_get_dpi();
  const short icon_size = 40 * UI_SCALE_FAC;
  const int dialog_width = icon_size + (style->widget.points * size * UI_SCALE_FAC);
  return uiItemsAlertBox(block, style, dialog_width, icon, icon_size);
}

/** \} */
