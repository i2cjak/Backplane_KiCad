/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "board_structure_fabrication.h"
#include <board.h>
#include <board_structure.h>
#include <base_units.h>
#include <backplane_document_metadata.h>
#include <plotters/plotter_gerber.h>
#include <reporter.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/filefn.h>
#include <cmath>

namespace
{
bool WriteText( const wxString& path, const std::string& data )
{
    wxFile file( path, wxFile::write );
    return file.IsOpened() && file.Write( data.data(), data.size() ) == data.size() && file.Close();
}

void Polygon( GERBER_PLOTTER& plot, const BOARD_STRUCTURE_POLYGON& shape, bool positive )
{
    plot.SetLayerPolarity( positive );
    plot.PlotPoly( shape.outline, FILL_T::FILLED_SHAPE, 0, nullptr );
    plot.SetLayerPolarity( !positive );
    for( const auto& hole : shape.holes )
        plot.PlotPoly( hole, FILL_T::FILLED_SHAPE, 0, nullptr );
    plot.SetLayerPolarity( true );
}
}

bool ExportBoardStructureFabrication( BOARD& board, const wxString& directory,
                                      const VECTOR2I& offset, REPORTER& reporter )
{
    const auto& structure = board.GetBoardStructure();
    if( structure.Empty() )
        return true;
    const auto validation = structure.Validate();
    if( !validation.IsValid() )
    {
        for( const auto& error : validation.errors )
            reporter.Report( wxString::FromUTF8( error ), RPT_SEVERITY_ERROR );
        return false;
    }
    if( !wxFileName::DirExists( directory )
        && !wxFileName::Mkdir( directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
        return false;

    nlohmann::json manifest = {
        { "format", "backplane-board-construction" }, { "version", 1 },
        { "units", "mm" }, { "state", "flat" },
        { "gerber_polarity", "dark is material; clear is removed material" },
        { "origin_nm", { offset.x, offset.y } },
        { "board", board.GetFileName().ToStdString() },
        { "structure", structure.ToJson() }, { "files", nlohmann::json::array() }
    };
    auto plot = [&]( const wxString& fileName, const std::string& role,
                     const std::string& id, const BOARD_STRUCTURE_POLYGON& shape,
                     const std::vector<BOARD_STRUCTURE_POLYGON>& openings )
    {
        const wxString path = wxFileName( directory, fileName ).GetFullPath();
        GERBER_PLOTTER p;
        p.SetViewport( offset, pcbIUScale.IU_PER_MILS / 10.0, 1.0, false );
        p.SetGerberCoordinatesFormat( 6, false );
        p.SetCreator( wxS( "Backplane KiCad" ) );
        if( !p.OpenFile( path ) || !p.StartPlot( wxEmptyString ) )
            return false;
        Polygon( p, shape, true );
        for( const auto& opening : openings )
            Polygon( p, opening, false );
        if( !p.EndPlot() )
            return false;
        manifest["files"].push_back( { { "path", fileName.ToStdString() },
                                        { "role", role }, { "object_id", id } } );
        return true;
    };
    size_t index = 0;
    for( const auto& region : structure.regions )
    {
        if( !plot( wxString::Format( "structure-region-%zu.gbr", ++index ),
                   "stack-region-material-area", region.id, region.shape, {} ) )
            return false;
    }
    index = 0;
    for( const auto& stiffener : structure.stiffeners )
    {
        const wxString name = wxString::Format( "structure-stiffener-%zu-%s.gbr", ++index,
                                                stiffener.top ? "top" : "bottom" );
        if( !plot( name, "stiffener-material-area", stiffener.id, stiffener.shape, {} ) )
            return false;
        if( stiffener.adhesive_thickness_nm > 0
            && !plot( wxString::Format( "structure-stiffener-%zu-adhesive.gbr", index ),
                       "stiffener-adhesive-material-area", stiffener.id, stiffener.shape, {} ) )
            return false;
    }
    index = 0;
    for( const auto& coverlay : structure.coverlays )
    {
        const wxString name = wxString::Format( "structure-coverlay-%zu-%s.gbr", ++index,
                                                coverlay.top ? "top" : "bottom" );
        if( !plot( name, "coverlay-material-area", coverlay.id,
                   coverlay.coverage, coverlay.openings ) )
            return false;
    }
    index = 0;
    for( const auto& bend : structure.bends )
    {
        double thickness = 0.0;
        for( const auto& region : structure.regions )
            if( region.id == bend.region_id )
                if( const auto* stack = structure.StackForRegion( region ) )
                    for( const auto& material : stack->materials )
                        thickness += material.thickness_nm * 1e-6;
        const double width = BOARD_STRUCTURE::BendStripLengthMm( bend, thickness ) * 1e6;
        const double dx = double( bend.end.x ) - bend.start.x;
        const double dy = double( bend.end.y ) - bend.start.y;
        const double length = std::hypot( dx, dy );
        const double sign = bend.moving_side_left ? 1.0 : -1.0;
        const VECTOR2I delta( KiROUND( -dy / length * sign * width ),
                              KiROUND( dx / length * sign * width ) );
        BOARD_STRUCTURE_POLYGON band;
        band.outline = { bend.start, bend.end, bend.end + delta, bend.start + delta };
        if( !plot( wxString::Format( "structure-bend-%zu.gbr", ++index ),
                   "bend-area-reference-only", bend.id, band, {} ) )
            return false;
    }
    manifest["fabrication_notes"] = {
        "Bend-area Gerbers are reference geometry, not material or machining layers.",
        "The native copper artwork and all construction Gerbers use the flat developed state.",
        "Material names, thicknesses, adhesive, bend radius and angle are in structure."
    };
    if( !WriteText( wxFileName( directory, "board-construction.json" ).GetFullPath(),
                    manifest.dump( 2 ) + "\n" ) )
        return false;
    reporter.Report( wxS( "Exported flat board construction Gerbers and manifest" ), RPT_SEVERITY_INFO );
    return true;
}
