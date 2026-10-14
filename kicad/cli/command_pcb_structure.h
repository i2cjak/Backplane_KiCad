/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "command.h"

namespace CLI
{
class PCB_STRUCTURE_COMMAND : public COMMAND
{
public:
    PCB_STRUCTURE_COMMAND();
protected:
    int doPerform( KIWAY& aKiway ) override;
};
}
