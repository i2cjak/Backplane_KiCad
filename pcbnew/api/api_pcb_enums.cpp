/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <import_export.h>
#include <api/api_enums.h>
#include <api/board/board_types.pb.h>
#include <api/board/board_commands.pb.h>
#include <api/board/board_jobs.pb.h>
#include <api/common/types/enums.pb.h>
#include <wx/wx.h>
#include <widgets/report_severity.h>

#include <board_stackup_manager/board_stackup.h>
#include <padstack.h>
#include <pcb_dimension.h>
#include <pcb_track.h>
#include <jobs/job_export_pcb_3d.h>
#include <jobs/job_export_pcb_dxf.h>
#include <jobs/job_export_pcb_drill.h>
#include <jobs/job_export_pcb_ipc2581.h>
#include <jobs/job_export_pcb_odb.h>
#include <jobs/job_export_pcb_pdf.h>
#include <jobs/job_export_pcb_pos.h>
#include <jobs/job_export_pcb_ps.h>
#include <jobs/job_export_pcb_stats.h>
#include <jobs/job_export_pcb_svg.h>
#include <jobs/job_pcb_render.h>
#include <plotprint_opts.h>
#include <zones.h>
#include <zone_settings.h>
#include <project/board_project_settings.h>

// Adding something new here?  Add it to test_api_enums.cpp!

using namespace kiapi::board;
using namespace kiapi::board::commands;
using namespace kiapi::board::jobs;

template<>
types::PadType ToProtoEnum( PAD_ATTRIB aValue )
{
    switch( aValue )
    {
    case PAD_ATTRIB::PTH:   return types::PadType::PT_PTH;
    case PAD_ATTRIB::SMD:   return types::PadType::PT_SMD;
    case PAD_ATTRIB::CONN:  return types::PadType::PT_EDGE_CONNECTOR;
    case PAD_ATTRIB::NPTH:  return types::PadType::PT_NPTH;

    default:
        wxCHECK_MSG( false, types::PadType::PT_UNKNOWN,
                     "Unhandled case in ToProtoEnum<PAD_ATTRIB>");
    }
}


template<>
PAD_ATTRIB FromProtoEnum( types::PadType aValue )
{
    switch( aValue )
    {
    case types::PadType::PT_UNKNOWN:
    case types::PadType::PT_PTH:            return PAD_ATTRIB::PTH;
    case types::PadType::PT_SMD:            return PAD_ATTRIB::SMD;
    case types::PadType::PT_EDGE_CONNECTOR: return PAD_ATTRIB::CONN;
    case types::PadType::PT_NPTH:           return PAD_ATTRIB::NPTH;

    default:
        wxCHECK_MSG( false,  PAD_ATTRIB::PTH,
                     "Unhandled case in FromProtoEnum<types::PadType>" );
    }
}

template<>
types::DrillShape ToProtoEnum( PAD_DRILL_SHAPE aValue )
{
    switch( aValue )
    {
    case PAD_DRILL_SHAPE::CIRCLE:    return types::DrillShape::DS_CIRCLE;
    case PAD_DRILL_SHAPE::OBLONG:    return types::DrillShape::DS_OBLONG;
    case PAD_DRILL_SHAPE::UNDEFINED: return types::DrillShape::DS_UNDEFINED;
    default:
        wxCHECK_MSG( false, types::DrillShape::DS_UNKNOWN,
                     "Unhandled case in ToProtoEnum<PAD_DRILL_SHAPE>");
    }
}

template<>
PAD_DRILL_SHAPE FromProtoEnum( types::DrillShape aValue )
{
    switch( aValue )
    {
    case types::DrillShape::DS_CIRCLE:      return PAD_DRILL_SHAPE::CIRCLE;
    case types::DrillShape::DS_OBLONG:      return PAD_DRILL_SHAPE::OBLONG;
    case types::DrillShape::DS_UNKNOWN:
    case types::DrillShape::DS_UNDEFINED:   return PAD_DRILL_SHAPE::UNDEFINED;
    default:
        wxCHECK_MSG( false, PAD_DRILL_SHAPE::UNDEFINED,
                     "Unhandled case in FromProtoEnum<types::DrillShape>" );
    }
}

template<>
types::PadStackShape ToProtoEnum( PAD_SHAPE aValue )
{
    switch( aValue )
    {
    case PAD_SHAPE::CIRCLE:         return types::PadStackShape::PSS_CIRCLE;
    case PAD_SHAPE::RECTANGLE:      return types::PadStackShape::PSS_RECTANGLE;
    case PAD_SHAPE::OVAL:           return types::PadStackShape::PSS_OVAL;
    case PAD_SHAPE::TRAPEZOID:      return types::PadStackShape::PSS_TRAPEZOID;
    case PAD_SHAPE::ROUNDRECT:      return types::PadStackShape::PSS_ROUNDRECT;
    case PAD_SHAPE::CHAMFERED_RECT: return types::PadStackShape::PSS_CHAMFEREDRECT;
    case PAD_SHAPE::CUSTOM:         return types::PadStackShape::PSS_CUSTOM;

    default:
        wxCHECK_MSG( false, types::PadStackShape::PSS_UNKNOWN,
                     "Unhandled case in ToProtoEnum<PAD_SHAPE>");
    }
}


template<>
PAD_SHAPE FromProtoEnum( types::PadStackShape aValue )
{
    switch( aValue )
    {
    case types::PadStackShape::PSS_UNKNOWN:
    case types::PadStackShape::PSS_CIRCLE:         return PAD_SHAPE::CIRCLE;
    case types::PadStackShape::PSS_RECTANGLE:      return PAD_SHAPE::RECTANGLE;
    case types::PadStackShape::PSS_OVAL:           return PAD_SHAPE::OVAL;
    case types::PadStackShape::PSS_TRAPEZOID:      return PAD_SHAPE::TRAPEZOID;
    case types::PadStackShape::PSS_ROUNDRECT:      return PAD_SHAPE::ROUNDRECT;
    case types::PadStackShape::PSS_CHAMFEREDRECT:  return PAD_SHAPE::CHAMFERED_RECT;
    case types::PadStackShape::PSS_CUSTOM:         return PAD_SHAPE::CUSTOM;

    default:
        wxCHECK_MSG( false, PAD_SHAPE::CIRCLE,
                     "Unhandled case in FromProtoEnum<types::PadStackShape>" );
    }
}


template<>
types::PadStackType ToProtoEnum( PADSTACK::MODE aValue )
{
    switch( aValue )
    {
    case PADSTACK::MODE::NORMAL:           return types::PadStackType::PST_NORMAL;
    case PADSTACK::MODE::FRONT_INNER_BACK: return types::PadStackType::PST_FRONT_INNER_BACK;
    case PADSTACK::MODE::CUSTOM:           return types::PadStackType::PST_CUSTOM;

    default:
        wxCHECK_MSG( false, types::PadStackType::PST_UNKNOWN,
                     "Unhandled case in ToProtoEnum<PADSTACK::MODE>");
    }
}


template<>
PADSTACK::MODE FromProtoEnum( types::PadStackType aValue )
{
    switch( aValue )
    {
    case types::PadStackType::PST_UNKNOWN:
    case types::PadStackType::PST_NORMAL:           return PADSTACK::MODE::NORMAL;
    case types::PadStackType::PST_FRONT_INNER_BACK: return PADSTACK::MODE::FRONT_INNER_BACK;
    case types::PadStackType::PST_CUSTOM:           return PADSTACK::MODE::CUSTOM;

    default:
        wxCHECK_MSG( false, PADSTACK::MODE::NORMAL,
                     "Unhandled case in FromProtoEnum<types::PadStackType>" );
    }
}


template<>
types::ViaType ToProtoEnum( VIATYPE aValue )
{
    switch( aValue )
    {
    case VIATYPE::THROUGH:      return types::ViaType::VT_THROUGH;
    case VIATYPE::BLIND:        return types::ViaType::VT_BLIND; // Since V10
    case VIATYPE::BURIED:       return types::ViaType::VT_BURIED;
    case VIATYPE::MICROVIA:     return types::ViaType::VT_MICRO;

    default:
        wxCHECK_MSG( false, types::ViaType::VT_UNKNOWN,
                     "Unhandled case in ToProtoEnum<VIATYPE>");
    }
}


template<>
VIATYPE FromProtoEnum( types::ViaType aValue )
{
    switch( aValue )
    {
    case types::ViaType::VT_UNKNOWN:
    case types::ViaType::VT_THROUGH:      return VIATYPE::THROUGH;
    case types::ViaType::VT_BLIND_BURIED: return VIATYPE::BLIND;
    case types::ViaType::VT_BLIND:        return VIATYPE::BLIND;    // Since V10
    case types::ViaType::VT_BURIED:       return VIATYPE::BURIED;
    case types::ViaType::VT_MICRO:        return VIATYPE::MICROVIA;

    default:
        wxCHECK_MSG( false, VIATYPE::THROUGH,
                     "Unhandled case in FromProtoEnum<types::ViaType>" );
    }
}


template<>
types::ZoneConnectionStyle ToProtoEnum( ZONE_CONNECTION aValue )
{
    switch( aValue )
    {
    case ZONE_CONNECTION::INHERITED:    return types::ZoneConnectionStyle::ZCS_INHERITED;
    case ZONE_CONNECTION::NONE:         return types::ZoneConnectionStyle::ZCS_NONE;
    case ZONE_CONNECTION::THERMAL:      return types::ZoneConnectionStyle::ZCS_THERMAL;
    case ZONE_CONNECTION::FULL:         return types::ZoneConnectionStyle::ZCS_FULL;
    case ZONE_CONNECTION::THT_THERMAL:  return types::ZoneConnectionStyle::ZCS_PTH_THERMAL;

    default:
        wxCHECK_MSG( false, types::ZoneConnectionStyle::ZCS_UNKNOWN,
                     "Unhandled case in ToProtoEnum<ZONE_CONNECTION>");
    }
}


template<>
ZONE_CONNECTION FromProtoEnum( types::ZoneConnectionStyle aValue )
{
    switch( aValue )
    {
    case types::ZoneConnectionStyle::ZCS_UNKNOWN:      return ZONE_CONNECTION::INHERITED;
    case types::ZoneConnectionStyle::ZCS_INHERITED:    return ZONE_CONNECTION::INHERITED;
    case types::ZoneConnectionStyle::ZCS_NONE:         return ZONE_CONNECTION::NONE;
    case types::ZoneConnectionStyle::ZCS_THERMAL:      return ZONE_CONNECTION::THERMAL;
    case types::ZoneConnectionStyle::ZCS_FULL:         return ZONE_CONNECTION::FULL;
    case types::ZoneConnectionStyle::ZCS_PTH_THERMAL:  return ZONE_CONNECTION::THT_THERMAL;

    default:
        wxCHECK_MSG( false, ZONE_CONNECTION::INHERITED,
                     "Unhandled case in FromProtoEnum<types::ZoneConnectionStyle>" );
    }
}


template<>
types::UnconnectedLayerRemoval ToProtoEnum( UNCONNECTED_LAYER_MODE aValue )
{
    switch( aValue )
    {
    case UNCONNECTED_LAYER_MODE::KEEP_ALL:
        return types::UnconnectedLayerRemoval::ULR_KEEP;

    case UNCONNECTED_LAYER_MODE::REMOVE_ALL:
        return types::UnconnectedLayerRemoval::ULR_REMOVE;

    case UNCONNECTED_LAYER_MODE::REMOVE_EXCEPT_START_AND_END:
        return types::UnconnectedLayerRemoval::ULR_REMOVE_EXCEPT_START_AND_END;

    case UNCONNECTED_LAYER_MODE::START_END_ONLY:
        return types::UnconnectedLayerRemoval::ULR_START_END_ONLY;

    default:
        wxCHECK_MSG( false, types::UnconnectedLayerRemoval::ULR_UNKNOWN,
                     "Unhandled case in ToProtoEnum<PADSTACK::UNCONNECTED_LAYER_MODE>");
    }
}


template<>
UNCONNECTED_LAYER_MODE FromProtoEnum( types::UnconnectedLayerRemoval aValue )
{
    switch( aValue )
    {
    case types::UnconnectedLayerRemoval::ULR_UNKNOWN:
    case types::UnconnectedLayerRemoval::ULR_KEEP:
        return UNCONNECTED_LAYER_MODE::KEEP_ALL;

    case types::UnconnectedLayerRemoval::ULR_REMOVE:
        return UNCONNECTED_LAYER_MODE::REMOVE_ALL;

    case types::UnconnectedLayerRemoval::ULR_REMOVE_EXCEPT_START_AND_END:
        return UNCONNECTED_LAYER_MODE::REMOVE_EXCEPT_START_AND_END;

    case types::UnconnectedLayerRemoval::ULR_START_END_ONLY:
        return UNCONNECTED_LAYER_MODE::START_END_ONLY;

    default:
        wxCHECK_MSG( false, UNCONNECTED_LAYER_MODE::KEEP_ALL,
                     "Unhandled case in FromProtoEnum<types::UnconnectedLayerRemoval>");
    }
}


template<>
types::IslandRemovalMode ToProtoEnum( ISLAND_REMOVAL_MODE aValue )
{
    switch( aValue )
    {
    case ISLAND_REMOVAL_MODE::ALWAYS:   return types::IslandRemovalMode::IRM_ALWAYS;
    case ISLAND_REMOVAL_MODE::NEVER:    return types::IslandRemovalMode::IRM_NEVER;
    case ISLAND_REMOVAL_MODE::AREA:     return types::IslandRemovalMode::IRM_AREA;

    default:
        wxCHECK_MSG( false, types::IslandRemovalMode::IRM_UNKNOWN,
                     "Unhandled case in ToProtoEnum<ISLAND_REMOVAL_MODE>");
    }
}


template<>
ISLAND_REMOVAL_MODE FromProtoEnum( types::IslandRemovalMode aValue )
{
    switch( aValue )
    {
    case types::IslandRemovalMode::IRM_UNKNOWN:
    case types::IslandRemovalMode::IRM_ALWAYS:  return ISLAND_REMOVAL_MODE::ALWAYS;
    case types::IslandRemovalMode::IRM_NEVER:   return ISLAND_REMOVAL_MODE::NEVER;
    case types::IslandRemovalMode::IRM_AREA:    return ISLAND_REMOVAL_MODE::AREA;

    default:
        wxCHECK_MSG( false, ISLAND_REMOVAL_MODE::ALWAYS,
                     "Unhandled case in FromProtoEnum<types::IslandRemovalMode>" );
    }
}


template<>
types::ZoneFillMode ToProtoEnum( ZONE_FILL_MODE aValue )
{
    switch( aValue )
    {
    case ZONE_FILL_MODE::POLYGONS:      return types::ZoneFillMode::ZFM_SOLID;
    case ZONE_FILL_MODE::HATCH_PATTERN: return types::ZoneFillMode::ZFM_HATCHED;

    default:
        wxCHECK_MSG( false, types::ZoneFillMode::ZFM_UNKNOWN,
                     "Unhandled case in ToProtoEnum<ZONE_FILL_MODE>");
    }
}


template<>
ZONE_FILL_MODE FromProtoEnum( types::ZoneFillMode aValue )
{
    switch( aValue )
    {
    case types::ZoneFillMode::ZFM_UNKNOWN:
    case types::ZoneFillMode::ZFM_SOLID:    return ZONE_FILL_MODE::POLYGONS;
    case types::ZoneFillMode::ZFM_HATCHED:  return ZONE_FILL_MODE::HATCH_PATTERN;

    default:
        wxCHECK_MSG( false, ZONE_FILL_MODE::POLYGONS,
                     "Unhandled case in FromProtoEnum<types::ZoneFillMode>" );
    }
}


template<>
types::ZoneBorderStyle ToProtoEnum( ZONE_BORDER_DISPLAY_STYLE aValue )
{
    switch( aValue )
    {
    case ZONE_BORDER_DISPLAY_STYLE::NO_HATCH:         return types::ZoneBorderStyle::ZBS_SOLID;
    case ZONE_BORDER_DISPLAY_STYLE::DIAGONAL_FULL:    return types::ZoneBorderStyle::ZBS_DIAGONAL_FULL;
    case ZONE_BORDER_DISPLAY_STYLE::DIAGONAL_EDGE:    return types::ZoneBorderStyle::ZBS_DIAGONAL_EDGE;
    case ZONE_BORDER_DISPLAY_STYLE::INVISIBLE_BORDER: return types::ZoneBorderStyle::ZBS_INVISIBLE;

    default:
        wxCHECK_MSG( false, types::ZoneBorderStyle::ZBS_UNKNOWN,
                     "Unhandled case in ToProtoEnum<ZONE_BORDER_DISPLAY_STYLE>");
    }
}


template<>
ZONE_BORDER_DISPLAY_STYLE FromProtoEnum( types::ZoneBorderStyle aValue )
{
    switch( aValue )
    {
    case types::ZoneBorderStyle::ZBS_SOLID:         return ZONE_BORDER_DISPLAY_STYLE::NO_HATCH;
    case types::ZoneBorderStyle::ZBS_DIAGONAL_FULL: return ZONE_BORDER_DISPLAY_STYLE::DIAGONAL_FULL;
    case types::ZoneBorderStyle::ZBS_UNKNOWN:
    case types::ZoneBorderStyle::ZBS_DIAGONAL_EDGE: return ZONE_BORDER_DISPLAY_STYLE::DIAGONAL_EDGE;
    case types::ZoneBorderStyle::ZBS_INVISIBLE:     return ZONE_BORDER_DISPLAY_STYLE::INVISIBLE_BORDER;

    default:
        wxCHECK_MSG( false, ZONE_BORDER_DISPLAY_STYLE::DIAGONAL_EDGE,
                     "Unhandled case in FromProtoEnum<types::ZoneHatchBorderMode>" );
    }
}


template<>
types::PlacementRuleSourceType ToProtoEnum( PLACEMENT_SOURCE_T aValue )
{
    switch( aValue )
    {
    case PLACEMENT_SOURCE_T::SHEETNAME:
        return types::PlacementRuleSourceType::PRST_SHEET_NAME;

    case PLACEMENT_SOURCE_T::COMPONENT_CLASS:
        return types::PlacementRuleSourceType::PRST_COMPONENT_CLASS;

    case PLACEMENT_SOURCE_T::GROUP_PLACEMENT:
        return types::PlacementRuleSourceType::PRST_GROUP;

    case PLACEMENT_SOURCE_T::DESIGN_BLOCK:
        return types::PlacementRuleSourceType::PRST_DESIGN_BLOCK;

    default:
        wxCHECK_MSG( false, types::PlacementRuleSourceType::PRST_UNKNOWN,
                     "Unhandled case in ToProtoEnum<PLACEMENT_SOURCE_T>");
    }
}


template<>
PLACEMENT_SOURCE_T FromProtoEnum( types::PlacementRuleSourceType aValue )
{
    switch( aValue )
    {
    case types::PlacementRuleSourceType::PRST_UNKNOWN:
    case types::PlacementRuleSourceType::PRST_SHEET_NAME:
        return PLACEMENT_SOURCE_T::SHEETNAME;

    case types::PlacementRuleSourceType::PRST_COMPONENT_CLASS:
        return PLACEMENT_SOURCE_T::COMPONENT_CLASS;

    case types::PlacementRuleSourceType::PRST_GROUP:
        return PLACEMENT_SOURCE_T::GROUP_PLACEMENT;

    case types::PlacementRuleSourceType::PRST_DESIGN_BLOCK:
        return PLACEMENT_SOURCE_T::DESIGN_BLOCK;

    default:
        wxCHECK_MSG( false, PLACEMENT_SOURCE_T::SHEETNAME,
                     "Unhandled case in FromProtoEnum<types::PlacementRuleSourceType>" );
    }
}


template<>
types::TeardropType ToProtoEnum( TEARDROP_TYPE aValue )
{
    switch( aValue )
    {
    case TEARDROP_TYPE::TD_NONE:        return types::TeardropType::TDT_NONE;
    case TEARDROP_TYPE::TD_UNSPECIFIED: return types::TeardropType::TDT_UNSPECIFIED;
    case TEARDROP_TYPE::TD_VIAPAD:      return types::TeardropType::TDT_VIA_PAD;
    case TEARDROP_TYPE::TD_TRACKEND:    return types::TeardropType::TDT_TRACK_END;

    default:
        wxCHECK_MSG( false, types::TeardropType::TDT_UNKNOWN,
                     "Unhandled case in ToProtoEnum<TEARDROP_TYPE>");
    }
}


template<>
TEARDROP_TYPE FromProtoEnum( types::TeardropType aValue )
{
    switch( aValue )
    {
    case types::TeardropType::TDT_UNKNOWN:
    case types::TeardropType::TDT_NONE:         return TEARDROP_TYPE::TD_NONE;
    case types::TeardropType::TDT_UNSPECIFIED:  return TEARDROP_TYPE::TD_UNSPECIFIED;
    case types::TeardropType::TDT_VIA_PAD:      return TEARDROP_TYPE::TD_VIAPAD;
    case types::TeardropType::TDT_TRACK_END:    return TEARDROP_TYPE::TD_TRACKEND;

    default:
        wxCHECK_MSG( false, TEARDROP_TYPE::TD_NONE,
                     "Unhandled case in FromProtoEnum<types::ZoneHatchBorderMode>" );
    }
}


template<>
types::DimensionTextBorderStyle ToProtoEnum( DIM_TEXT_BORDER aValue )
{
    switch( aValue )
    {
    case DIM_TEXT_BORDER::NONE:         return types::DimensionTextBorderStyle::DTBS_NONE;
    case DIM_TEXT_BORDER::RECTANGLE:    return types::DimensionTextBorderStyle::DTBS_RECTANGLE;
    case DIM_TEXT_BORDER::CIRCLE:       return types::DimensionTextBorderStyle::DTBS_CIRCLE;
    case DIM_TEXT_BORDER::ROUNDRECT:    return types::DimensionTextBorderStyle::DTBS_ROUNDRECT;

    default:
        wxCHECK_MSG( false, types::DimensionTextBorderStyle::DTBS_UNKNOWN,
                     "Unhandled case in ToProtoEnum<DIM_TEXT_BORDER>");
    }
}


template<>
DIM_TEXT_BORDER FromProtoEnum( types::DimensionTextBorderStyle aValue )
{
    switch( aValue )
    {
    case types::DimensionTextBorderStyle::DTBS_UNKNOWN:
    case types::DimensionTextBorderStyle::DTBS_NONE:        return DIM_TEXT_BORDER::NONE;
    case types::DimensionTextBorderStyle::DTBS_RECTANGLE:   return DIM_TEXT_BORDER::RECTANGLE;
    case types::DimensionTextBorderStyle::DTBS_CIRCLE:      return DIM_TEXT_BORDER::CIRCLE;
    case types::DimensionTextBorderStyle::DTBS_ROUNDRECT:   return DIM_TEXT_BORDER::ROUNDRECT;

    default:
        wxCHECK_MSG( false,  DIM_TEXT_BORDER::NONE,
                     "Unhandled case in FromProtoEnum<types::DimensionTextBorderStyle>" );
    }
}


template<>
types::DimensionUnitFormat ToProtoEnum( DIM_UNITS_FORMAT aValue )
{
    switch( aValue )
    {
    case DIM_UNITS_FORMAT::NO_SUFFIX:       return types::DimensionUnitFormat::DUF_NO_SUFFIX;
    case DIM_UNITS_FORMAT::BARE_SUFFIX:     return types::DimensionUnitFormat::DUF_BARE_SUFFIX;
    case DIM_UNITS_FORMAT::PAREN_SUFFIX:    return types::DimensionUnitFormat::DUF_PAREN_SUFFIX;

    default:
        wxCHECK_MSG( false, types::DimensionUnitFormat::DUF_UNKNOWN,
                     "Unhandled case in ToProtoEnum<DIM_UNITS_FORMAT>");
    }
}


template<>
DIM_UNITS_FORMAT FromProtoEnum( types::DimensionUnitFormat aValue )
{
    switch( aValue )
    {
    case types::DimensionUnitFormat::DUF_UNKNOWN:
    case types::DimensionUnitFormat::DUF_NO_SUFFIX:     return DIM_UNITS_FORMAT::NO_SUFFIX;
    case types::DimensionUnitFormat::DUF_BARE_SUFFIX:   return DIM_UNITS_FORMAT::BARE_SUFFIX;
    case types::DimensionUnitFormat::DUF_PAREN_SUFFIX:  return DIM_UNITS_FORMAT::PAREN_SUFFIX;

    default:
        wxCHECK_MSG( false,  DIM_UNITS_FORMAT::NO_SUFFIX,
                     "Unhandled case in FromProtoEnum<types::DimensionUnitFormat>" );
    }
}


template<>
types::DimensionArrowDirection ToProtoEnum( DIM_ARROW_DIRECTION aValue )
{
    switch( aValue )
    {
    case DIM_ARROW_DIRECTION::INWARD:   return types::DimensionArrowDirection::DAD_INWARD;
    case DIM_ARROW_DIRECTION::OUTWARD:  return types::DimensionArrowDirection::DAD_OUTWARD;

    default:
        wxCHECK_MSG( false, types::DimensionArrowDirection::DAD_UNKNOWN,
                     "Unhandled case in ToProtoEnum<DIM_ARROW_DIRECTION>");
    }
}


template<>
DIM_ARROW_DIRECTION FromProtoEnum( types::DimensionArrowDirection aValue )
{
    switch( aValue )
    {
    case types::DimensionArrowDirection::DAD_INWARD:    return DIM_ARROW_DIRECTION::INWARD;
    case types::DimensionArrowDirection::DAD_UNKNOWN:
    case types::DimensionArrowDirection::DAD_OUTWARD:   return DIM_ARROW_DIRECTION::OUTWARD;

    default:
        wxCHECK_MSG( false,  DIM_ARROW_DIRECTION::OUTWARD,
                     "Unhandled case in FromProtoEnum<types::DimensionArrowDirection>" );
    }
}


template<>
types::DimensionPrecision ToProtoEnum( DIM_PRECISION aValue )
{
    switch( aValue )
    {
    case DIM_PRECISION::X:          return types::DimensionPrecision::DP_FIXED_0;
    case DIM_PRECISION::X_X:        return types::DimensionPrecision::DP_FIXED_1;
    case DIM_PRECISION::X_XX:       return types::DimensionPrecision::DP_FIXED_2;
    case DIM_PRECISION::X_XXX:      return types::DimensionPrecision::DP_FIXED_3;
    case DIM_PRECISION::X_XXXX:     return types::DimensionPrecision::DP_FIXED_4;
    case DIM_PRECISION::X_XXXXX:    return types::DimensionPrecision::DP_FIXED_5;
    case DIM_PRECISION::V_VV:       return types::DimensionPrecision::DP_SCALED_IN_2;
    case DIM_PRECISION::V_VVV:      return types::DimensionPrecision::DP_SCALED_IN_3;
    case DIM_PRECISION::V_VVVV:     return types::DimensionPrecision::DP_SCALED_IN_4;
    case DIM_PRECISION::V_VVVVV:    return types::DimensionPrecision::DP_SCALED_IN_5;

    default:
        wxCHECK_MSG( false, types::DimensionPrecision::DP_UNKNOWN,
                     "Unhandled case in ToProtoEnum<DIM_PRECISION>");
    }
}


template<>
DIM_PRECISION FromProtoEnum( types::DimensionPrecision aValue )
{
    switch( aValue )
    {
    case types::DimensionPrecision::DP_FIXED_0:     return DIM_PRECISION::X;
    case types::DimensionPrecision::DP_FIXED_1:     return DIM_PRECISION::X_X;
    case types::DimensionPrecision::DP_FIXED_2:     return DIM_PRECISION::X_XX;
    case types::DimensionPrecision::DP_FIXED_3:     return DIM_PRECISION::X_XXX;
    case types::DimensionPrecision::DP_FIXED_4:     return DIM_PRECISION::X_XXXX;
    case types::DimensionPrecision::DP_FIXED_5:     return DIM_PRECISION::X_XXXXX;
    case types::DimensionPrecision::DP_UNKNOWN:
    case types::DimensionPrecision::DP_SCALED_IN_2: return DIM_PRECISION::V_VV;
    case types::DimensionPrecision::DP_SCALED_IN_3: return DIM_PRECISION::V_VVV;
    case types::DimensionPrecision::DP_SCALED_IN_4: return DIM_PRECISION::V_VVVV;
    case types::DimensionPrecision::DP_SCALED_IN_5: return DIM_PRECISION::V_VVVVV;

    default:
        wxCHECK_MSG( false,  DIM_PRECISION::V_VV,
                     "Unhandled case in FromProtoEnum<types::DimensionPrecision>" );
    }
}


template<>
types::DimensionTextPosition ToProtoEnum( DIM_TEXT_POSITION aValue )
{
    switch( aValue )
    {
    case DIM_TEXT_POSITION::OUTSIDE:    return types::DimensionTextPosition::DTP_OUTSIDE;
    case DIM_TEXT_POSITION::INLINE:     return types::DimensionTextPosition::DTP_INLINE;
    case DIM_TEXT_POSITION::MANUAL:     return types::DimensionTextPosition::DTP_MANUAL;

    default:
        wxCHECK_MSG( false, types::DimensionTextPosition::DTP_UNKNOWN,
                     "Unhandled case in ToProtoEnum<DIM_TEXT_POSITION>");
    }
}


template<>
DIM_TEXT_POSITION FromProtoEnum( types::DimensionTextPosition aValue )
{
    switch( aValue )
    {
    case types::DimensionTextPosition::DTP_UNKNOWN:
    case types::DimensionTextPosition::DTP_OUTSIDE: return DIM_TEXT_POSITION::OUTSIDE;
    case types::DimensionTextPosition::DTP_INLINE:  return DIM_TEXT_POSITION::INLINE;
    case types::DimensionTextPosition::DTP_MANUAL:  return DIM_TEXT_POSITION::MANUAL;

    default:
        wxCHECK_MSG( false,  DIM_TEXT_POSITION::OUTSIDE,
                     "Unhandled case in FromProtoEnum<types::DimensionTextPosition>" );
    }
}


template<>
types::DimensionUnit ToProtoEnum( DIM_UNITS_MODE aValue )
{
    switch( aValue )
    {
    case DIM_UNITS_MODE::INCH:      return types::DimensionUnit::DU_INCHES;
    case DIM_UNITS_MODE::MILS:      return types::DimensionUnit::DU_MILS;
    case DIM_UNITS_MODE::MM:        return types::DimensionUnit::DU_MILLIMETERS;
    case DIM_UNITS_MODE::AUTOMATIC: return types::DimensionUnit::DU_AUTOMATIC;

    default:
        wxCHECK_MSG( false, types::DimensionUnit::DU_UNKNOWN,
                     "Unhandled case in ToProtoEnum<DIM_UNITS_MODE>");
    }
}


template<>
DIM_UNITS_MODE FromProtoEnum( types::DimensionUnit aValue )
{
    switch( aValue )
    {
    case types::DimensionUnit::DU_INCHES:       return DIM_UNITS_MODE::INCH;
    case types::DimensionUnit::DU_MILS:         return DIM_UNITS_MODE::MILS;
    case types::DimensionUnit::DU_MILLIMETERS:  return DIM_UNITS_MODE::MM;
    case types::DimensionUnit::DU_UNKNOWN:
    case types::DimensionUnit::DU_AUTOMATIC:    return DIM_UNITS_MODE::AUTOMATIC;

    default:
        wxCHECK_MSG( false,  DIM_UNITS_MODE::AUTOMATIC,
                     "Unhandled case in FromProtoEnum<types::DimensionUnit>" );
    }
}


template<>
commands::InactiveLayerDisplayMode ToProtoEnum( HIGH_CONTRAST_MODE aValue )
{
    switch( aValue )
    {
    case HIGH_CONTRAST_MODE::NORMAL:    return commands::InactiveLayerDisplayMode::ILDM_NORMAL;
    case HIGH_CONTRAST_MODE::DIMMED:    return commands::InactiveLayerDisplayMode::ILDM_DIMMED;
    case HIGH_CONTRAST_MODE::HIDDEN:    return commands::InactiveLayerDisplayMode::ILDM_HIDDEN;

    default:
        wxCHECK_MSG( false, commands::InactiveLayerDisplayMode::ILDM_NORMAL,
                     "Unhandled case in ToProtoEnum<HIGH_CONTRAST_MODE>");
    }
}


template<>
HIGH_CONTRAST_MODE FromProtoEnum( commands::InactiveLayerDisplayMode aValue )
{
    switch( aValue )
    {
    case commands::InactiveLayerDisplayMode::ILDM_DIMMED:   return HIGH_CONTRAST_MODE::DIMMED;
    case commands::InactiveLayerDisplayMode::ILDM_HIDDEN:   return HIGH_CONTRAST_MODE::HIDDEN;
    case commands::InactiveLayerDisplayMode::ILDM_UNKNOWN:
    case commands::InactiveLayerDisplayMode::ILDM_NORMAL:   return HIGH_CONTRAST_MODE::NORMAL;

    default:
        wxCHECK_MSG( false, HIGH_CONTRAST_MODE::NORMAL,
                     "Unhandled case in FromProtoEnum<commands::InactiveLayerDisplayMode>" );
    }
}


template<>
commands::NetColorDisplayMode ToProtoEnum( NET_COLOR_MODE aValue )
{
    switch( aValue )
    {
    case NET_COLOR_MODE::ALL:       return commands::NetColorDisplayMode::NCDM_ALL;
    case NET_COLOR_MODE::RATSNEST:  return commands::NetColorDisplayMode::NCDM_RATSNEST;
    case NET_COLOR_MODE::OFF:       return commands::NetColorDisplayMode::NCDM_OFF;

    default:
        wxCHECK_MSG( false, commands::NetColorDisplayMode::NCDM_UNKNOWN,
                     "Unhandled case in ToProtoEnum<NET_COLOR_MODE>");
    }
}


template<>
NET_COLOR_MODE FromProtoEnum( commands::NetColorDisplayMode aValue )
{
    switch( aValue )
    {
    case commands::NetColorDisplayMode::NCDM_ALL:       return NET_COLOR_MODE::ALL;
    case commands::NetColorDisplayMode::NCDM_OFF:       return NET_COLOR_MODE::OFF;
    case commands::NetColorDisplayMode::NCDM_UNKNOWN:
    case commands::NetColorDisplayMode::NCDM_RATSNEST:  return NET_COLOR_MODE::RATSNEST;

    default:
        wxCHECK_MSG( false, NET_COLOR_MODE::RATSNEST,
                     "Unhandled case in FromProtoEnum<commands::NetColorDisplayMode>" );
    }
}


template<>
commands::RatsnestDisplayMode ToProtoEnum( RATSNEST_MODE aValue )
{
    switch( aValue )
    {
    case RATSNEST_MODE::ALL:        return commands::RatsnestDisplayMode::RDM_ALL_LAYERS;
    case RATSNEST_MODE::VISIBLE:    return commands::RatsnestDisplayMode::RDM_VISIBLE_LAYERS;

    default:
        wxCHECK_MSG( false, commands::RatsnestDisplayMode::RDM_UNKNOWN,
                     "Unhandled case in ToProtoEnum<RATSNEST_MODE>");
    }
}


template<>
RATSNEST_MODE FromProtoEnum( commands::RatsnestDisplayMode aValue )
{
    switch( aValue )
    {
    case commands::RatsnestDisplayMode::RDM_VISIBLE_LAYERS: return RATSNEST_MODE::VISIBLE;
    case commands::RatsnestDisplayMode::RDM_UNKNOWN:
    case commands::RatsnestDisplayMode::RDM_ALL_LAYERS:     return RATSNEST_MODE::ALL;

    default:
        wxCHECK_MSG( false, RATSNEST_MODE::ALL,
                     "Unhandled case in FromProtoEnum<commands::RatsnestDisplayMode>" );
    }
}


template<>
BoardStackupLayerType ToProtoEnum( BOARD_STACKUP_ITEM_TYPE aValue )
{
    switch( aValue )
    {
    case BS_ITEM_TYPE_UNDEFINED:    return BoardStackupLayerType::BSLT_UNDEFINED;
    case BS_ITEM_TYPE_COPPER:       return BoardStackupLayerType::BSLT_COPPER;
    case BS_ITEM_TYPE_DIELECTRIC:   return BoardStackupLayerType::BSLT_DIELECTRIC;
    case BS_ITEM_TYPE_SOLDERPASTE:  return BoardStackupLayerType::BSLT_SOLDERPASTE;
    case BS_ITEM_TYPE_SOLDERMASK:   return BoardStackupLayerType::BSLT_SOLDERMASK;
    case BS_ITEM_TYPE_SILKSCREEN:   return BoardStackupLayerType::BSLT_SILKSCREEN;

    default:
        wxCHECK_MSG( false, BoardStackupLayerType::BSLT_UNKNOWN,
                     "Unhandled case in ToProtoEnum<BOARD_STACKUP_ITEM_TYPE>");
    }
}


template<>
BOARD_STACKUP_ITEM_TYPE FromProtoEnum( BoardStackupLayerType aValue )
{
    switch( aValue )
    {
    case BoardStackupLayerType::BSLT_UNDEFINED:    return BS_ITEM_TYPE_UNDEFINED;
    case BoardStackupLayerType::BSLT_COPPER:       return BS_ITEM_TYPE_COPPER;
    case BoardStackupLayerType::BSLT_DIELECTRIC:   return BS_ITEM_TYPE_DIELECTRIC;
    case BoardStackupLayerType::BSLT_SOLDERPASTE:  return BS_ITEM_TYPE_SOLDERPASTE;
    case BoardStackupLayerType::BSLT_SOLDERMASK:   return BS_ITEM_TYPE_SOLDERMASK;
    case BoardStackupLayerType::BSLT_SILKSCREEN:   return BS_ITEM_TYPE_SILKSCREEN;

    default:
        wxCHECK_MSG( false, BS_ITEM_TYPE_UNDEFINED,
                     "Unhandled case in FromProtoEnum<BoardStackupLayerType>" );
    }
}


template<>
BoardEdgeConnectorType ToProtoEnum( BS_EDGE_CONNECTOR_CONSTRAINTS aValue )
{
    switch( aValue )
    {
    case BS_EDGE_CONNECTOR_NONE:     return BoardEdgeConnectorType::BECT_NONE;
    case BS_EDGE_CONNECTOR_IN_USE:   return BoardEdgeConnectorType::BECT_PLAIN;
    case BS_EDGE_CONNECTOR_BEVELLED: return BoardEdgeConnectorType::BECT_BEVELED;

    default:
        wxCHECK_MSG( false, BoardEdgeConnectorType::BECT_UNKNOWN,
                     "Unhandled case in ToProtoEnum<BS_EDGE_CONNECTOR_CONSTRAINTS>" );
    }
}


template<>
BS_EDGE_CONNECTOR_CONSTRAINTS FromProtoEnum( BoardEdgeConnectorType aValue )
{
    switch( aValue )
    {
    case BoardEdgeConnectorType::BECT_NONE:    return BS_EDGE_CONNECTOR_NONE;
    case BoardEdgeConnectorType::BECT_PLAIN:   return BS_EDGE_CONNECTOR_IN_USE;
    case BoardEdgeConnectorType::BECT_BEVELED: return BS_EDGE_CONNECTOR_BEVELLED;

    default:
        wxCHECK_MSG( false, BS_EDGE_CONNECTOR_NONE,
                     "Unhandled case in FromProtoEnum<BoardEdgeConnectorType>" );
    }
}


template<>
DrcSeverity ToProtoEnum( SEVERITY aValue )
{
    switch( aValue )
    {
    case RPT_SEVERITY_WARNING:   return DrcSeverity::DRS_WARNING;
    case RPT_SEVERITY_ERROR:     return DrcSeverity::DRS_ERROR;
    case RPT_SEVERITY_EXCLUSION: return DrcSeverity::DRS_EXCLUSION;
    case RPT_SEVERITY_IGNORE:    return DrcSeverity::DRS_IGNORE;
    case RPT_SEVERITY_INFO:      return DrcSeverity::DRS_INFO;
    case RPT_SEVERITY_ACTION:    return DrcSeverity::DRS_ACTION;
    case RPT_SEVERITY_DEBUG:     return DrcSeverity::DRS_DEBUG;
    case RPT_SEVERITY_UNDEFINED: return DrcSeverity::DRS_UNDEFINED;
    default:
        wxCHECK_MSG( false, DrcSeverity::DRS_UNDEFINED,
                     "Unhandled case in ToProtoEnum<SEVERITY>");
    }
}


template<>
SEVERITY FromProtoEnum( DrcSeverity aValue )
{
    switch( aValue )
    {
    case DrcSeverity::DRS_WARNING:   return RPT_SEVERITY_WARNING;
    case DrcSeverity::DRS_ERROR:     return RPT_SEVERITY_ERROR;
    case DrcSeverity::DRS_EXCLUSION: return RPT_SEVERITY_EXCLUSION;
    case DrcSeverity::DRS_IGNORE:    return RPT_SEVERITY_IGNORE;
    case DrcSeverity::DRS_INFO:      return RPT_SEVERITY_INFO;
    case DrcSeverity::DRS_ACTION:    return RPT_SEVERITY_ACTION;
    case DrcSeverity::DRS_DEBUG:     return RPT_SEVERITY_DEBUG;
    case DrcSeverity::DRS_UNKNOWN:
    default:
        return RPT_SEVERITY_UNDEFINED;
    }
}

template<>
PlotDrillMarks ToProtoEnum( DRILL_MARKS aValue )
{
    switch( aValue )
    {
    case DRILL_MARKS::NO_DRILL_SHAPE:    return PlotDrillMarks::PDM_NONE;
    case DRILL_MARKS::SMALL_DRILL_SHAPE: return PlotDrillMarks::PDM_SMALL;
    case DRILL_MARKS::FULL_DRILL_SHAPE:  return PlotDrillMarks::PDM_FULL;
    default:
        wxCHECK_MSG( false, PlotDrillMarks::PDM_UNKNOWN,
                     "Unhandled case in ToProtoEnum<DRILL_MARKS>" );
    }
}


template<>
DRILL_MARKS FromProtoEnum( PlotDrillMarks aValue )
{
    switch( aValue )
    {
    case PlotDrillMarks::PDM_NONE:    return DRILL_MARKS::NO_DRILL_SHAPE;
    case PlotDrillMarks::PDM_SMALL:   return DRILL_MARKS::SMALL_DRILL_SHAPE;
    case PlotDrillMarks::PDM_FULL:    return DRILL_MARKS::FULL_DRILL_SHAPE;
    case PlotDrillMarks::PDM_UNKNOWN:
    default:
        return DRILL_MARKS::NO_DRILL_SHAPE;
    }
}


template<>
Board3DFormat ToProtoEnum( JOB_EXPORT_PCB_3D::FORMAT aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_3D::FORMAT::STEP:    return Board3DFormat::B3D_STEP;
    case JOB_EXPORT_PCB_3D::FORMAT::STEPZ:   return Board3DFormat::B3D_STEPZ;
    case JOB_EXPORT_PCB_3D::FORMAT::BREP:    return Board3DFormat::B3D_BREP;
    case JOB_EXPORT_PCB_3D::FORMAT::XAO:     return Board3DFormat::B3D_XAO;
    case JOB_EXPORT_PCB_3D::FORMAT::GLB:     return Board3DFormat::B3D_GLB;
    case JOB_EXPORT_PCB_3D::FORMAT::VRML:    return Board3DFormat::B3D_VRML;
    case JOB_EXPORT_PCB_3D::FORMAT::PLY:     return Board3DFormat::B3D_PLY;
    case JOB_EXPORT_PCB_3D::FORMAT::STL:     return Board3DFormat::B3D_STL;
    case JOB_EXPORT_PCB_3D::FORMAT::U3D:     return Board3DFormat::B3D_U3D;
    case JOB_EXPORT_PCB_3D::FORMAT::PDF:     return Board3DFormat::B3D_PDF;
    default:
        wxCHECK_MSG( false, Board3DFormat::B3D_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_3D::FORMAT>" );
    }
}


template<>
JOB_EXPORT_PCB_3D::FORMAT FromProtoEnum( Board3DFormat aValue )
{
    switch( aValue )
    {
    case Board3DFormat::B3D_STEP:  return JOB_EXPORT_PCB_3D::FORMAT::STEP;
    case Board3DFormat::B3D_STEPZ: return JOB_EXPORT_PCB_3D::FORMAT::STEPZ;
    case Board3DFormat::B3D_BREP:  return JOB_EXPORT_PCB_3D::FORMAT::BREP;
    case Board3DFormat::B3D_XAO:   return JOB_EXPORT_PCB_3D::FORMAT::XAO;
    case Board3DFormat::B3D_GLB:   return JOB_EXPORT_PCB_3D::FORMAT::GLB;
    case Board3DFormat::B3D_VRML:  return JOB_EXPORT_PCB_3D::FORMAT::VRML;
    case Board3DFormat::B3D_PLY:   return JOB_EXPORT_PCB_3D::FORMAT::PLY;
    case Board3DFormat::B3D_STL:   return JOB_EXPORT_PCB_3D::FORMAT::STL;
    case Board3DFormat::B3D_U3D:   return JOB_EXPORT_PCB_3D::FORMAT::U3D;
    case Board3DFormat::B3D_PDF:   return JOB_EXPORT_PCB_3D::FORMAT::PDF;
    case Board3DFormat::B3D_UNKNOWN:
    default:
        return JOB_EXPORT_PCB_3D::FORMAT::UNKNOWN;
    }
}


template<>
kiapi::common::types::Units ToProtoEnum( JOB_EXPORT_PCB_3D::VRML_UNITS aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_3D::VRML_UNITS::INCH:   return kiapi::common::types::Units::U_INCH;
    case JOB_EXPORT_PCB_3D::VRML_UNITS::MM:     return kiapi::common::types::Units::U_MM;
    case JOB_EXPORT_PCB_3D::VRML_UNITS::METERS: return kiapi::common::types::Units::U_METERS;
    case JOB_EXPORT_PCB_3D::VRML_UNITS::TENTHS: return kiapi::common::types::Units::U_TENTHS;
    default:
        wxCHECK_MSG( false, kiapi::common::types::Units::U_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_3D::VRML_UNITS>" );
    }
}


template<>
JOB_EXPORT_PCB_3D::VRML_UNITS FromProtoEnum( kiapi::common::types::Units aValue )
{
    switch( aValue )
    {
    case kiapi::common::types::Units::U_INCH:   return JOB_EXPORT_PCB_3D::VRML_UNITS::INCH;
    case kiapi::common::types::Units::U_MM:     return JOB_EXPORT_PCB_3D::VRML_UNITS::MM;
    case kiapi::common::types::Units::U_METERS: return JOB_EXPORT_PCB_3D::VRML_UNITS::METERS;
    case kiapi::common::types::Units::U_TENTHS: return JOB_EXPORT_PCB_3D::VRML_UNITS::TENTHS;
    case kiapi::common::types::Units::U_UNKNOWN:
    default:
        return JOB_EXPORT_PCB_3D::VRML_UNITS::METERS;
    }
}


template<>
RenderFormat ToProtoEnum( JOB_PCB_RENDER::FORMAT aValue )
{
    switch( aValue )
    {
    case JOB_PCB_RENDER::FORMAT::PNG:  return RenderFormat::RF_PNG;
    case JOB_PCB_RENDER::FORMAT::JPEG: return RenderFormat::RF_JPEG;
    default:
        wxCHECK_MSG( false, RenderFormat::RF_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_PCB_RENDER::FORMAT>" );
    }
}


template<>
JOB_PCB_RENDER::FORMAT FromProtoEnum( RenderFormat aValue )
{
    switch( aValue )
    {
    case RenderFormat::RF_PNG: return JOB_PCB_RENDER::FORMAT::PNG;
    case RenderFormat::RF_JPEG:return JOB_PCB_RENDER::FORMAT::JPEG;
    case RenderFormat::RF_UNKNOWN:
    default:
        return JOB_PCB_RENDER::FORMAT::PNG;
    }
}


template<>
RenderQuality ToProtoEnum( JOB_PCB_RENDER::QUALITY aValue )
{
    switch( aValue )
    {
    case JOB_PCB_RENDER::QUALITY::BASIC:        return RenderQuality::RQ_BASIC;
    case JOB_PCB_RENDER::QUALITY::HIGH:         return RenderQuality::RQ_HIGH;
    case JOB_PCB_RENDER::QUALITY::USER:         return RenderQuality::RQ_USER;
    case JOB_PCB_RENDER::QUALITY::JOB_SETTINGS: return RenderQuality::RQ_JOB_SETTINGS;
    default:
        wxCHECK_MSG( false, RenderQuality::RQ_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_PCB_RENDER::QUALITY>" );
    }
}


template<>
JOB_PCB_RENDER::QUALITY FromProtoEnum( RenderQuality aValue )
{
    switch( aValue )
    {
    case RenderQuality::RQ_BASIC:        return JOB_PCB_RENDER::QUALITY::BASIC;
    case RenderQuality::RQ_HIGH:         return JOB_PCB_RENDER::QUALITY::HIGH;
    case RenderQuality::RQ_USER:         return JOB_PCB_RENDER::QUALITY::USER;
    case RenderQuality::RQ_JOB_SETTINGS: return JOB_PCB_RENDER::QUALITY::JOB_SETTINGS;
    case RenderQuality::RQ_UNKNOWN:
    default:
        return JOB_PCB_RENDER::QUALITY::BASIC;
    }
}


template<>
RenderBackgroundStyle ToProtoEnum( JOB_PCB_RENDER::BG_STYLE aValue )
{
    switch( aValue )
    {
    case JOB_PCB_RENDER::BG_STYLE::DEFAULT:     return RenderBackgroundStyle::RBS_DEFAULT;
    case JOB_PCB_RENDER::BG_STYLE::TRANSPARENT: return RenderBackgroundStyle::RBS_TRANSPARENT;
    case JOB_PCB_RENDER::BG_STYLE::OPAQUE:      return RenderBackgroundStyle::RBS_OPAQUE;
    default:
        wxCHECK_MSG( false, RenderBackgroundStyle::RBS_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_PCB_RENDER::BG_STYLE>" );
    }
}


template<>
JOB_PCB_RENDER::BG_STYLE FromProtoEnum( RenderBackgroundStyle aValue )
{
    switch( aValue )
    {
    case RenderBackgroundStyle::RBS_DEFAULT:     return JOB_PCB_RENDER::BG_STYLE::DEFAULT;
    case RenderBackgroundStyle::RBS_TRANSPARENT: return JOB_PCB_RENDER::BG_STYLE::TRANSPARENT;
    case RenderBackgroundStyle::RBS_OPAQUE:      return JOB_PCB_RENDER::BG_STYLE::OPAQUE;
    case RenderBackgroundStyle::RBS_UNKNOWN:
    default:
        return JOB_PCB_RENDER::BG_STYLE::DEFAULT;
    }
}


template<>
RenderSide ToProtoEnum( JOB_PCB_RENDER::SIDE aValue )
{
    switch( aValue )
    {
    case JOB_PCB_RENDER::SIDE::TOP:    return RenderSide::RS_TOP;
    case JOB_PCB_RENDER::SIDE::BOTTOM: return RenderSide::RS_BOTTOM;
    case JOB_PCB_RENDER::SIDE::LEFT:   return RenderSide::RS_LEFT;
    case JOB_PCB_RENDER::SIDE::RIGHT:  return RenderSide::RS_RIGHT;
    case JOB_PCB_RENDER::SIDE::FRONT:  return RenderSide::RS_FRONT;
    case JOB_PCB_RENDER::SIDE::BACK:   return RenderSide::RS_BACK;
    default:
        wxCHECK_MSG( false, RenderSide::RS_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_PCB_RENDER::SIDE>" );
    }
}


template<>
JOB_PCB_RENDER::SIDE FromProtoEnum( RenderSide aValue )
{
    switch( aValue )
    {
    case RenderSide::RS_TOP:    return JOB_PCB_RENDER::SIDE::TOP;
    case RenderSide::RS_BOTTOM: return JOB_PCB_RENDER::SIDE::BOTTOM;
    case RenderSide::RS_LEFT:   return JOB_PCB_RENDER::SIDE::LEFT;
    case RenderSide::RS_RIGHT:  return JOB_PCB_RENDER::SIDE::RIGHT;
    case RenderSide::RS_FRONT:  return JOB_PCB_RENDER::SIDE::FRONT;
    case RenderSide::RS_BACK:   return JOB_PCB_RENDER::SIDE::BACK;
    case RenderSide::RS_UNKNOWN:
    default:
        return JOB_PCB_RENDER::SIDE::TOP;
    }
}


template<>
BoardJobPaginationMode ToProtoEnum( JOB_EXPORT_PCB_SVG::GEN_MODE aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_SVG::GEN_MODE::SINGLE: return BoardJobPaginationMode::BJPM_ALL_LAYERS_ONE_PAGE;
    case JOB_EXPORT_PCB_SVG::GEN_MODE::MULTI:  return BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_FILE;
    default:
        wxCHECK_MSG( false, BoardJobPaginationMode::BJPM_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_SVG::GEN_MODE>" );
    }
}


template<>
JOB_EXPORT_PCB_SVG::GEN_MODE FromProtoEnum( BoardJobPaginationMode aValue )
{
    switch( aValue )
    {
    case BoardJobPaginationMode::BJPM_ALL_LAYERS_ONE_PAGE:
        return JOB_EXPORT_PCB_SVG::GEN_MODE::SINGLE;
    case BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_FILE:
        return JOB_EXPORT_PCB_SVG::GEN_MODE::MULTI;
    case BoardJobPaginationMode::BJPM_UNKNOWN:
    case BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_PAGE:
    default:
        return JOB_EXPORT_PCB_SVG::GEN_MODE::SINGLE;
    }
}


template<>
BoardJobPaginationMode ToProtoEnum( JOB_EXPORT_PCB_DXF::GEN_MODE aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_DXF::GEN_MODE::SINGLE: return BoardJobPaginationMode::BJPM_ALL_LAYERS_ONE_PAGE;
    case JOB_EXPORT_PCB_DXF::GEN_MODE::MULTI:  return BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_FILE;
    default:
        wxCHECK_MSG( false, BoardJobPaginationMode::BJPM_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_DXF::GEN_MODE>" );
    }
}


template<>
JOB_EXPORT_PCB_DXF::GEN_MODE FromProtoEnum( BoardJobPaginationMode aValue )
{
    switch( aValue )
    {
    case BoardJobPaginationMode::BJPM_ALL_LAYERS_ONE_PAGE:
        return JOB_EXPORT_PCB_DXF::GEN_MODE::SINGLE;
    case BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_FILE:
        return JOB_EXPORT_PCB_DXF::GEN_MODE::MULTI;
    case BoardJobPaginationMode::BJPM_UNKNOWN:
    case BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_PAGE:
    default:
        return JOB_EXPORT_PCB_DXF::GEN_MODE::SINGLE;
    }
}


template<>
BoardJobPaginationMode ToProtoEnum( JOB_EXPORT_PCB_PDF::GEN_MODE aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_PDF::GEN_MODE::ALL_LAYERS_ONE_FILE:
        return BoardJobPaginationMode::BJPM_ALL_LAYERS_ONE_PAGE;
    case JOB_EXPORT_PCB_PDF::GEN_MODE::ONE_PAGE_PER_LAYER_ONE_FILE:
        return BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_PAGE;
    case JOB_EXPORT_PCB_PDF::GEN_MODE::ALL_LAYERS_SEPARATE_FILE:
        return BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_FILE;
    default:
        wxCHECK_MSG( false, BoardJobPaginationMode::BJPM_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_PDF::GEN_MODE>" );
    }
}


template<>
JOB_EXPORT_PCB_PDF::GEN_MODE FromProtoEnum( BoardJobPaginationMode aValue )
{
    switch( aValue )
    {
    case BoardJobPaginationMode::BJPM_ALL_LAYERS_ONE_PAGE:
        return JOB_EXPORT_PCB_PDF::GEN_MODE::ALL_LAYERS_ONE_FILE;
    case BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_PAGE:
        return JOB_EXPORT_PCB_PDF::GEN_MODE::ONE_PAGE_PER_LAYER_ONE_FILE;
    case BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_FILE:
        return JOB_EXPORT_PCB_PDF::GEN_MODE::ALL_LAYERS_SEPARATE_FILE;
    case BoardJobPaginationMode::BJPM_UNKNOWN:
    default:
        return JOB_EXPORT_PCB_PDF::GEN_MODE::ALL_LAYERS_ONE_FILE;
    }
}


template<>
BoardJobPaginationMode ToProtoEnum( JOB_EXPORT_PCB_PS::GEN_MODE aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_PS::GEN_MODE::SINGLE: return BoardJobPaginationMode::BJPM_ALL_LAYERS_ONE_PAGE;
    case JOB_EXPORT_PCB_PS::GEN_MODE::MULTI:  return BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_FILE;
    default:
        wxCHECK_MSG( false, BoardJobPaginationMode::BJPM_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_PS::GEN_MODE>" );
    }
}


template<>
JOB_EXPORT_PCB_PS::GEN_MODE FromProtoEnum( BoardJobPaginationMode aValue )
{
    switch( aValue )
    {
    case BoardJobPaginationMode::BJPM_ALL_LAYERS_ONE_PAGE:
        return JOB_EXPORT_PCB_PS::GEN_MODE::SINGLE;
    case BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_FILE:
        return JOB_EXPORT_PCB_PS::GEN_MODE::MULTI;
    case BoardJobPaginationMode::BJPM_UNKNOWN:
    case BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_PAGE:
    default:
        return JOB_EXPORT_PCB_PS::GEN_MODE::SINGLE;
    }
}


template<>
DrillFormat ToProtoEnum( JOB_EXPORT_PCB_DRILL::DRILL_FORMAT aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_DRILL::DRILL_FORMAT::EXCELLON: return DrillFormat::DF_EXCELLON;
    case JOB_EXPORT_PCB_DRILL::DRILL_FORMAT::GERBER:   return DrillFormat::DF_GERBER;
    default:
        wxCHECK_MSG( false, DrillFormat::DF_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_DRILL::DRILL_FORMAT>" );
    }
}


template<>
JOB_EXPORT_PCB_DRILL::DRILL_FORMAT FromProtoEnum( DrillFormat aValue )
{
    switch( aValue )
    {
    case DrillFormat::DF_EXCELLON: return JOB_EXPORT_PCB_DRILL::DRILL_FORMAT::EXCELLON;
    case DrillFormat::DF_GERBER:   return JOB_EXPORT_PCB_DRILL::DRILL_FORMAT::GERBER;
    case DrillFormat::DF_UNKNOWN:
    default:
        return JOB_EXPORT_PCB_DRILL::DRILL_FORMAT::EXCELLON;
    }
}


template<>
PositionSide ToProtoEnum( JOB_EXPORT_PCB_POS::SIDE aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_POS::SIDE::FRONT: return PositionSide::PS_FRONT;
    case JOB_EXPORT_PCB_POS::SIDE::BACK:  return PositionSide::PS_BACK;
    case JOB_EXPORT_PCB_POS::SIDE::BOTH:  return PositionSide::PS_BOTH;
    default:
        wxCHECK_MSG( false, PositionSide::PS_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_POS::SIDE>" );
    }
}


template<>
JOB_EXPORT_PCB_POS::SIDE FromProtoEnum( PositionSide aValue )
{
    switch( aValue )
    {
    case PositionSide::PS_FRONT: return JOB_EXPORT_PCB_POS::SIDE::FRONT;
    case PositionSide::PS_BACK:  return JOB_EXPORT_PCB_POS::SIDE::BACK;
    case PositionSide::PS_BOTH:  return JOB_EXPORT_PCB_POS::SIDE::BOTH;
    case PositionSide::PS_UNKNOWN:
    default:
        return JOB_EXPORT_PCB_POS::SIDE::BOTH;
    }
}


template<>
PositionFormat ToProtoEnum( JOB_EXPORT_PCB_POS::FORMAT aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_POS::FORMAT::ASCII:  return PositionFormat::PF_ASCII;
    case JOB_EXPORT_PCB_POS::FORMAT::CSV:    return PositionFormat::PF_CSV;
    case JOB_EXPORT_PCB_POS::FORMAT::GERBER: return PositionFormat::PF_GERBER;
    default:
        wxCHECK_MSG( false, PositionFormat::PF_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_POS::FORMAT>" );
    }
}


template<>
JOB_EXPORT_PCB_POS::FORMAT FromProtoEnum( PositionFormat aValue )
{
    switch( aValue )
    {
    case PositionFormat::PF_ASCII:  return JOB_EXPORT_PCB_POS::FORMAT::ASCII;
    case PositionFormat::PF_CSV:    return JOB_EXPORT_PCB_POS::FORMAT::CSV;
    case PositionFormat::PF_GERBER: return JOB_EXPORT_PCB_POS::FORMAT::GERBER;
    case PositionFormat::PF_UNKNOWN:
    default:
        return JOB_EXPORT_PCB_POS::FORMAT::ASCII;
    }
}


template<>
Ipc2581Version ToProtoEnum( JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION::B: return Ipc2581Version::IPC2581V_B;
    case JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION::C: return Ipc2581Version::IPC2581V_C;
    default:
        wxCHECK_MSG( false, Ipc2581Version::IPC2581V_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION>" );
    }
}


template<>
JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION FromProtoEnum( Ipc2581Version aValue )
{
    switch( aValue )
    {
    case Ipc2581Version::IPC2581V_B: return JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION::B;
    case Ipc2581Version::IPC2581V_C: return JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION::C;
    case Ipc2581Version::IPC2581V_UNKNOWN:
    default:
        return JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION::C;
    }
}


template<>
OdbCompression ToProtoEnum( JOB_EXPORT_PCB_ODB::ODB_COMPRESSION aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_ODB::ODB_COMPRESSION::NONE: return OdbCompression::ODBC_NONE;
    case JOB_EXPORT_PCB_ODB::ODB_COMPRESSION::ZIP:  return OdbCompression::ODBC_ZIP;
    case JOB_EXPORT_PCB_ODB::ODB_COMPRESSION::TGZ:  return OdbCompression::ODBC_TGZ;
    default:
        wxCHECK_MSG( false, OdbCompression::ODBC_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_ODB::ODB_COMPRESSION>" );
    }
}


template<>
JOB_EXPORT_PCB_ODB::ODB_COMPRESSION FromProtoEnum( OdbCompression aValue )
{
    switch( aValue )
    {
    case OdbCompression::ODBC_NONE: return JOB_EXPORT_PCB_ODB::ODB_COMPRESSION::NONE;
    case OdbCompression::ODBC_ZIP:  return JOB_EXPORT_PCB_ODB::ODB_COMPRESSION::ZIP;
    case OdbCompression::ODBC_TGZ:  return JOB_EXPORT_PCB_ODB::ODB_COMPRESSION::TGZ;
    case OdbCompression::ODBC_UNKNOWN:
    default:
        return JOB_EXPORT_PCB_ODB::ODB_COMPRESSION::ZIP;
    }
}


template<>
StatsOutputFormat ToProtoEnum( JOB_EXPORT_PCB_STATS::OUTPUT_FORMAT aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_STATS::OUTPUT_FORMAT::REPORT: return StatsOutputFormat::SOF_REPORT;
    case JOB_EXPORT_PCB_STATS::OUTPUT_FORMAT::JSON:   return StatsOutputFormat::SOF_JSON;
    default:
        wxCHECK_MSG( false, StatsOutputFormat::SOF_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_STATS::OUTPUT_FORMAT>" );
    }
}


template<>
JOB_EXPORT_PCB_STATS::OUTPUT_FORMAT FromProtoEnum( StatsOutputFormat aValue )
{
    switch( aValue )
    {
    case StatsOutputFormat::SOF_REPORT: return JOB_EXPORT_PCB_STATS::OUTPUT_FORMAT::REPORT;
    case StatsOutputFormat::SOF_JSON:   return JOB_EXPORT_PCB_STATS::OUTPUT_FORMAT::JSON;
    case StatsOutputFormat::SOF_UNKNOWN:
    default:
        return JOB_EXPORT_PCB_STATS::OUTPUT_FORMAT::REPORT;
    }
}


template<>
kiapi::common::types::Units ToProtoEnum( JOB_EXPORT_PCB_DXF::DXF_UNITS aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_DXF::DXF_UNITS::INCH: return kiapi::common::types::Units::U_INCH;
    case JOB_EXPORT_PCB_DXF::DXF_UNITS::MM:   return kiapi::common::types::Units::U_MM;
    default:
        wxCHECK_MSG( false, kiapi::common::types::Units::U_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_DXF::DXF_UNITS>" );
    }
}


template<>
JOB_EXPORT_PCB_DXF::DXF_UNITS FromProtoEnum( kiapi::common::types::Units aValue )
{
    switch( aValue )
    {
    case kiapi::common::types::Units::U_INCH: return JOB_EXPORT_PCB_DXF::DXF_UNITS::INCH;
    case kiapi::common::types::Units::U_MM:   return JOB_EXPORT_PCB_DXF::DXF_UNITS::MM;
    case kiapi::common::types::Units::U_UNKNOWN:
    case kiapi::common::types::Units::U_METERS:
    case kiapi::common::types::Units::U_TENTHS:
    default:
        return JOB_EXPORT_PCB_DXF::DXF_UNITS::INCH;
    }
}


template<>
kiapi::common::types::Units ToProtoEnum( JOB_EXPORT_PCB_POS::UNITS aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_POS::UNITS::INCH: return kiapi::common::types::Units::U_INCH;
    case JOB_EXPORT_PCB_POS::UNITS::MM:   return kiapi::common::types::Units::U_MM;
    default:
        wxCHECK_MSG( false, kiapi::common::types::Units::U_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_POS::UNITS>" );
    }
}


template<>
JOB_EXPORT_PCB_POS::UNITS FromProtoEnum( kiapi::common::types::Units aValue )
{
    switch( aValue )
    {
    case kiapi::common::types::Units::U_INCH: return JOB_EXPORT_PCB_POS::UNITS::INCH;
    case kiapi::common::types::Units::U_MM:   return JOB_EXPORT_PCB_POS::UNITS::MM;
    case kiapi::common::types::Units::U_UNKNOWN:
    case kiapi::common::types::Units::U_METERS:
    case kiapi::common::types::Units::U_TENTHS:
    default:
        return JOB_EXPORT_PCB_POS::UNITS::MM;
    }
}


template<>
kiapi::common::types::Units ToProtoEnum( JOB_EXPORT_PCB_IPC2581::IPC2581_UNITS aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_IPC2581::IPC2581_UNITS::INCH: return kiapi::common::types::Units::U_INCH;
    case JOB_EXPORT_PCB_IPC2581::IPC2581_UNITS::MM:   return kiapi::common::types::Units::U_MM;
    default:
        wxCHECK_MSG( false, kiapi::common::types::Units::U_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_IPC2581::IPC2581_UNITS>" );
    }
}


template<>
JOB_EXPORT_PCB_IPC2581::IPC2581_UNITS FromProtoEnum( kiapi::common::types::Units aValue )
{
    switch( aValue )
    {
    case kiapi::common::types::Units::U_INCH: return JOB_EXPORT_PCB_IPC2581::IPC2581_UNITS::INCH;
    case kiapi::common::types::Units::U_MM:   return JOB_EXPORT_PCB_IPC2581::IPC2581_UNITS::MM;
    case kiapi::common::types::Units::U_UNKNOWN:
    case kiapi::common::types::Units::U_METERS:
    case kiapi::common::types::Units::U_TENTHS:
    default:
        return JOB_EXPORT_PCB_IPC2581::IPC2581_UNITS::MM;
    }
}


template<>
kiapi::common::types::Units ToProtoEnum( JOB_EXPORT_PCB_ODB::ODB_UNITS aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_ODB::ODB_UNITS::INCH: return kiapi::common::types::Units::U_INCH;
    case JOB_EXPORT_PCB_ODB::ODB_UNITS::MM:   return kiapi::common::types::Units::U_MM;
    default:
        wxCHECK_MSG( false, kiapi::common::types::Units::U_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_ODB::ODB_UNITS>" );
    }
}


template<>
JOB_EXPORT_PCB_ODB::ODB_UNITS FromProtoEnum( kiapi::common::types::Units aValue )
{
    switch( aValue )
    {
    case kiapi::common::types::Units::U_INCH: return JOB_EXPORT_PCB_ODB::ODB_UNITS::INCH;
    case kiapi::common::types::Units::U_MM:   return JOB_EXPORT_PCB_ODB::ODB_UNITS::MM;
    case kiapi::common::types::Units::U_UNKNOWN:
    case kiapi::common::types::Units::U_METERS:
    case kiapi::common::types::Units::U_TENTHS:
    default:
        return JOB_EXPORT_PCB_ODB::ODB_UNITS::MM;
    }
}


template<>
kiapi::common::types::Units ToProtoEnum( JOB_EXPORT_PCB_STATS::UNITS aValue )
{
    switch( aValue )
    {
    case JOB_EXPORT_PCB_STATS::UNITS::INCH: return kiapi::common::types::Units::U_INCH;
    case JOB_EXPORT_PCB_STATS::UNITS::MM:   return kiapi::common::types::Units::U_MM;
    default:
        wxCHECK_MSG( false, kiapi::common::types::Units::U_UNKNOWN,
                     "Unhandled case in ToProtoEnum<JOB_EXPORT_PCB_STATS::UNITS>" );
    }
}


template<>
JOB_EXPORT_PCB_STATS::UNITS FromProtoEnum( kiapi::common::types::Units aValue )
{
    switch( aValue )
    {
    case kiapi::common::types::Units::U_INCH: return JOB_EXPORT_PCB_STATS::UNITS::INCH;
    case kiapi::common::types::Units::U_MM:   return JOB_EXPORT_PCB_STATS::UNITS::MM;
    case kiapi::common::types::Units::U_UNKNOWN:
    case kiapi::common::types::Units::U_METERS:
    case kiapi::common::types::Units::U_TENTHS:
    default:
        return JOB_EXPORT_PCB_STATS::UNITS::MM;
    }
}

// Adding something new here?  Add it to test_api_enums.cpp!
