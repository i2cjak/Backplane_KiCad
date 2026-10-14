#pragma once
#include "command.h"
namespace CLI { class PCB_ASSEMBLY_COMMAND : public COMMAND { public: PCB_ASSEMBLY_COMMAND(); protected: int doPerform( KIWAY& ) override; }; }
