/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <string>

#include <board_structure.h>

class PCB_EDIT_FRAME;

/** Choose an existing physical structure stack or create a small starter
 * stack for a new board.  The board is not touched until the caller commits
 * the returned staged structure. */
bool ChooseStructureStack( PCB_EDIT_FRAME* aParent, BOARD_STRUCTURE& aStaged,
                           BOARD_STRUCTURE_STACK_KIND aKind, std::string& aOutputStackId,
                           bool aForceNew = false );
