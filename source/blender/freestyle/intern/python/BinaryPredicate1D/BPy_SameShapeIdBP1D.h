/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#pragma once

#include "../BPy_BinaryPredicate1D.h"

///////////////////////////////////////////////////////////////////////////////////////////

extern PyTypeObject SameShapeIdBP1D_Type;

#define BPy_SameShapeIdBP1D_Check(v) \
  (PyObject_IsInstance((PyObject *)v, (PyObject *)&SameShapeIdBP1D_Type))

/*---------------------------Python BPy_SameShapeIdBP1D structure definition----------*/
typedef struct {
  BPy_BinaryPredicate1D py_bp1D;
} BPy_SameShapeIdBP1D;

///////////////////////////////////////////////////////////////////////////////////////////
