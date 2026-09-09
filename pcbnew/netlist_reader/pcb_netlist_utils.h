/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 */

#ifndef KICAD_PCB_NETLIST_UTILS_H
#define KICAD_PCB_NETLIST_UTILS_H

class BOARD;
class FOOTPRINT;
class LIB_ID;
class NETLIST;
class REPORTER;

/** Load a footprint from the project footprint library table. */
FOOTPRINT* LoadFootprintFromProject( BOARD* aBoard, const LIB_ID& aFootprintId,
                                     bool aKeepUuid = false );

/** Preload netlist footprints so the updater can add or replace components. */
void LoadNetlistFootprints( BOARD* aBoard, NETLIST& aNetlist, REPORTER& aReporter );

#endif
