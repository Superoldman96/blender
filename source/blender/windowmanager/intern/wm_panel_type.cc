/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Panel Registry.
 *
 * \note Unlike menu, and other registries, this doesn't *own* the PanelType.
 *
 * For popups/popovers only, regions handle panel types by including them in local lists.
 */

#include <cstdio>

#include "DNA_windowmanager_types.h"

#include "BLI_utildefines.h"
#include "BLI_vector_set.hh"

#include "BKE_screen.hh"

#include "WM_api.hh"

using blender::StringRef;

struct PanelTypePointerHash {
  uint64_t operator()(const PanelType *value) const
  {
    return get_default_hash(StringRef(value->idname));
  }
  uint64_t operator()(const StringRef name) const
  {
    return get_default_hash(name);
  }
};

struct PanelTypePointerNameEqual {
  bool operator()(const PanelType *a, const PanelType *b) const
  {
    return STREQ(a->idname, b->idname);
  }
  bool operator()(const StringRef idname, const PanelType *a) const
  {
    return a->idname == idname;
  }
};

static auto &get_panel_type_map()
{
  static blender::VectorSet<PanelType *,
                            blender::DefaultProbingStrategy,
                            PanelTypePointerHash,
                            PanelTypePointerNameEqual>
      map;
  return map;
}

PanelType *WM_paneltype_find(const char *idname, bool quiet)
{
  if (idname[0]) {
    if (PanelType *const *pt = get_panel_type_map().lookup_key_ptr_as(StringRef(idname))) {
      return *pt;
    }
  }

  if (!quiet) {
    printf("search for unknown paneltype %s\n", idname);
  }

  return nullptr;
}

bool WM_paneltype_add(PanelType *pt)
{
  get_panel_type_map().add(pt);
  return true;
}

void WM_paneltype_remove(PanelType *pt)
{
  const bool ok = get_panel_type_map().remove(pt);
  BLI_assert(ok);
  UNUSED_VARS_NDEBUG(ok);
}

void WM_paneltype_init()
{
  /* Reserve size is set based on blender default setup. */
  get_panel_type_map().reserve(512);
}

void WM_paneltype_clear()
{
  get_panel_type_map().clear();
}

void WM_paneltype_idname_visit_for_search(
    const bContext * /*C*/,
    PointerRNA * /*ptr*/,
    PropertyRNA * /*prop*/,
    const char * /*edit_text*/,
    blender::FunctionRef<void(StringPropertySearchVisitParams)> visit_fn)
{
  for (PanelType *pt : get_panel_type_map()) {
    StringPropertySearchVisitParams visit_params{};
    visit_params.text = pt->idname;
    visit_params.info = pt->label;
    visit_fn(visit_params);
  }
}
