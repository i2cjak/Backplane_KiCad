/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "job.h"

class JOB_PCB_STRUCTURE : public JOB
{
public:
    JOB_PCB_STRUCTURE() : JOB( "pcb_structure", true ) {}
    wxString filename;
    bool exportFabrication = false;
};
