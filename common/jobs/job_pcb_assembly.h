#pragma once
#include <jobs/job.h>
class JOB_PCB_ASSEMBLY : public JOB
{
public:
    JOB_PCB_ASSEMBLY() : JOB( "pcb_assembly", true ) {}
    wxString filename;
    wxString output;
    bool check = false;
};
