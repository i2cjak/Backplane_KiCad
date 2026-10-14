// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Physical construction layers have their own identity: they must never be
// mistaken for native PCB_LAYER_ID copper layers by routers or file writers.
enum class CONSTRUCTION_LAYER
{
    NONE = 0,
    STACKUP_BOUNDARY,
    BEND,
    F_STIFFENER,
    B_STIFFENER
};

inline const char* ConstructionLayerName( CONSTRUCTION_LAYER aLayer )
{
    switch( aLayer )
    {
    case CONSTRUCTION_LAYER::STACKUP_BOUNDARY: return "Stackup.Boundary";
    case CONSTRUCTION_LAYER::BEND: return "Bend.Lines";
    case CONSTRUCTION_LAYER::F_STIFFENER: return "F.Stiffener";
    case CONSTRUCTION_LAYER::B_STIFFENER: return "B.Stiffener";
    default: return "";
    }
}

inline int ConstructionLayerCategory( CONSTRUCTION_LAYER aLayer )
{
    switch( aLayer )
    {
    case CONSTRUCTION_LAYER::STACKUP_BOUNDARY: return 4;
    case CONSTRUCTION_LAYER::BEND: return 3;
    case CONSTRUCTION_LAYER::F_STIFFENER: return 5;
    case CONSTRUCTION_LAYER::B_STIFFENER: return 6;
    default: return -1;
    }
}
