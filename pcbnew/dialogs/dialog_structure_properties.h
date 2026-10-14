// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
class wxWindow;
class BOARD_STRUCTURE;

// category: 0 region (rigid or flex), 2 stiffener, 3 bend. Edits staged data only.
bool EditStructureFeatureProperties( wxWindow* aParent, BOARD_STRUCTURE& aStructure,
                                     int aCategory, size_t aIndex );
