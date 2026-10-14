#include "panel_setup_rigid_flex.h"
#include "dialog_structure_stack_preset.h"

#include <pcb_edit_frame.h>
#include <board.h>
#include <board_structure.h>
#include <layer_ids.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <set>
#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/dialog.h>
#include <wx/grid.h>
#include <wx/notebook.h>
#include <wx/dcbuffer.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/msgdlg.h>
#include <wx/tokenzr.h>
#include <math/util.h>

namespace
{
enum
{
    ID_RF_ADD = wxID_HIGHEST + 141,
    ID_RF_DELETE
    , ID_RF_LAYERS
};

class STACK_LAYERS_DIALOG : public wxDialog
{
public:
    STACK_LAYERS_DIALOG( wxWindow* parent, BOARD_STRUCTURE_STACK& stack ) : wxDialog( parent, wxID_ANY, _( "Edit Stack Layers" ), wxDefaultPosition, wxSize( 1250, 450 ), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ), m_stack( stack ), m_working( stack )
    {
        auto* root = new wxBoxSizer( wxVERTICAL );
        m_grid = new wxGrid( this, wxID_ANY );
        m_grid->CreateGrid( 0, 7 );
        const wxString labels[] = { _( "Material ID" ), _( "Category / material" ), _( "Global copper ID (-1 none)" ), _( "Thickness (mm)" ), _( "Min (mm)" ), _( "Max (mm)" ), _( "Z offset (mm)" ) };
        const int widths[] = { 130, 250, 210, 130, 120, 120, 140 };
        m_grid->SetRowLabelSize( 40 );
        for( int i = 0; i < 7; ++i )
        {
            m_grid->SetColLabelValue( i, labels[i] );
            m_grid->SetColSize( i, widths[i] );
        }
        root->Add( m_grid, 1, wxEXPAND | wxALL, 6 );
        auto* row = new wxBoxSizer( wxHORIZONTAL );
        auto add = [&]( const wxString& label, int id ) { auto* b = new wxButton( this, id, label ); row->Add( b, 0, wxRIGHT, 4 ); Bind( wxEVT_BUTTON, &STACK_LAYERS_DIALOG::OnButton, this, id ); };
        add( _( "Add" ), wxID_ADD ); add( _( "Delete" ), wxID_REMOVE ); add( _( "Move up" ), wxID_UP ); add( _( "Move down" ), wxID_DOWN );
        row->AddStretchSpacer(); row->Add( new wxButton( this, wxID_CANCEL, _( "Cancel" ) ), 0, wxRIGHT, 4 ); row->Add( new wxButton( this, wxID_OK, _( "OK" ) ), 0 );
        root->Add( row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6 ); SetSizer( root );
        for( size_t i = 0; i < m_working.layers.size(); ++i ) AddLayer( i );
    }
private:
    void AddLayer( size_t row )
    {
        m_grid->AppendRows(); const auto& layer = m_working.layers[row];
        const BOARD_STRUCTURE_MATERIAL* material = nullptr;
        for( const auto& candidate : m_working.materials ) if( candidate.id == layer.material_id ) { material = &candidate; break; }
        m_grid->SetCellValue( row, 0, wxString::FromUTF8( layer.material_id ) );
        m_grid->SetCellValue( row, 1, material ? wxString::Format( "%s / %s", wxString::FromUTF8( material->category ), wxString::FromUTF8( material->material ) ) : wxString() );
        m_grid->SetCellValue( row, 2, wxString::Format( "%d", layer.global_copper_layer ) );
        m_grid->SetCellValue( row, 3, material ? wxString::Format( "%.4f", material->thickness_nm / 1e6 ) : wxString() );
        m_grid->SetCellValue( row, 4, material ? wxString::Format( "%.4f", material->min_thickness_nm / 1e6 ) : wxString() );
        m_grid->SetCellValue( row, 5, material ? wxString::Format( "%.4f", material->max_thickness_nm / 1e6 ) : wxString() );
        m_grid->SetCellValue( row, 6, wxString::Format( "%.4f", layer.z_offset_nm / 1e6 ) );
    }
    void OnButton( wxCommandEvent& event )
    {
        const int row = m_grid->GetGridCursorRow();
        if( event.GetId() == wxID_ADD ) { m_grid->AppendRows(); return; }
        if( row < 0 || row >= m_grid->GetNumberRows() ) return;
        if( event.GetId() == wxID_REMOVE ) { m_grid->DeleteRows( row ); return; }
        int other = event.GetId() == wxID_UP ? row - 1 : row + 1;
        if( other < 0 || other >= m_grid->GetNumberRows() ) return;
        for( int col = 0; col < 7; ++col ) { wxString value = m_grid->GetCellValue( row, col ); m_grid->SetCellValue( row, col, m_grid->GetCellValue( other, col ) ); m_grid->SetCellValue( other, col, value ); }
        m_grid->SetGridCursor( other, 0 );
    }
    bool Transfer()
    {
        m_error.clear();
        std::vector<BOARD_STRUCTURE_STACK::LAYER_INSTANCE> layers;
        m_grid->SaveEditControlValue();
        std::vector<BOARD_STRUCTURE_MATERIAL> materials;
        std::set<int> copperLayerSet;
        std::vector<std::pair<int64_t, int>> copperByHeight;
        std::set<std::string> materialIds;
        for( int row = 0; row < m_grid->GetNumberRows(); ++row )
        {
            auto invalid = [&]( const wxString& message )
            {
                m_error = wxString::Format( _( "Row %d: %s" ), row + 1, message );
                return false;
            };
            wxString id = m_grid->GetCellValue( row, 0 ).Trim(); wxString materialText = m_grid->GetCellValue( row, 1 );
            if( id.empty() || materialText.empty() ) return invalid( _( "material ID and category/material are required." ) );
            long copper = -1; double thickness = 0, minThickness = 0, maxThickness = 0, z = 0;
            if( !m_grid->GetCellValue( row, 2 ).ToLong( &copper ) || !m_grid->GetCellValue( row, 3 ).ToDouble( &thickness ) || !m_grid->GetCellValue( row, 4 ).ToDouble( &minThickness ) || !m_grid->GetCellValue( row, 5 ).ToDouble( &maxThickness ) || !m_grid->GetCellValue( row, 6 ).ToDouble( &z ) || !std::isfinite( thickness ) || !std::isfinite( minThickness ) || !std::isfinite( maxThickness ) || !std::isfinite( z ) || thickness <= 0 || minThickness < 0 || maxThickness < minThickness || ( maxThickness > 0 && ( minThickness > thickness || thickness > maxThickness ) ) ) return invalid( _( "thickness, min/max tolerances, and Z offset must be numeric and ordered." ) );
            if( copper != -1 && ( copper < 0 || copper >= PCB_LAYER_ID_COUNT || !IsCopperLayer( copper ) ) ) return invalid( _( "global copper ID must be -1 or a valid copper layer." ) );
            if( copper >= 0 && !copperLayerSet.insert( static_cast<int>( copper ) ).second ) return invalid( _( "global copper ID is duplicated." ) );
            BOARD_STRUCTURE_MATERIAL material;
            material.id = id.ToUTF8().data();
            if( !materialIds.insert( material.id ).second ) return invalid( _( "material ID is duplicated." ) );
            for( const auto& old : m_working.materials )
                if( old.id == material.id ) material = old;
            const wxString category = materialText.BeforeFirst( '/' ).Trim();
            material.category = category.ToUTF8().data();
            material.material = materialText.AfterFirst( '/' ).Trim().ToUTF8().data();
            if( material.material.empty() ) material.material = material.category;
            material.thickness_nm = static_cast<int64_t>( std::llround( thickness * 1e6 ) );
            material.min_thickness_nm = static_cast<int64_t>( std::llround( minThickness * 1e6 ) );
            material.max_thickness_nm = static_cast<int64_t>( std::llround( maxThickness * 1e6 ) );
            auto existing = std::find_if( materials.begin(), materials.end(), [&]( const auto& candidate ) { return candidate.id == material.id; } );
            if( existing != materials.end() ) *existing = material; else materials.push_back( material );
            layers.push_back( { material.id, static_cast<int>( copper ), static_cast<int64_t>( std::llround( z * 1e6 ) ) } );
            if( copper >= 0 )
                copperByHeight.emplace_back( static_cast<int64_t>( std::llround( z * 1e6 ) ),
                                              static_cast<int>( copper ) );
        }
        m_working.materials = std::move( materials );
        m_working.layers = std::move( layers );
        std::stable_sort( copperByHeight.begin(), copperByHeight.end(),
                          []( const auto& a, const auto& b ) { return a.first > b.first; } );
        m_working.copper_layers.clear();
        for( const auto& copper : copperByHeight )
            m_working.copper_layers.push_back( copper.second );
        m_stack = std::move( m_working );
        return true;
    }
    void EndModal( int retCode ) override { if( retCode == wxID_OK && !Transfer() ) { wxMessageBox( m_error.empty() ? _( "Every layer needs an ID, material, positive thickness, valid min/max tolerances, and numeric copper/Z values." ) : m_error, _( "Invalid stack layer" ), wxOK | wxICON_ERROR, this ); return; } wxDialog::EndModal( retCode ); }
    BOARD_STRUCTURE_STACK& m_stack; BOARD_STRUCTURE_STACK m_working; wxGrid* m_grid; wxString m_error;
};

wxString polygonText( const BOARD_STRUCTURE_POLYGON& polygon )
{
    wxString result;
    for( const VECTOR2I& point : polygon.outline )
    {
        if( !result.empty() ) result += "; ";
        result += wxString::Format( "%.4f,%.4f", point.x / 1e6, point.y / 1e6 );
    }
    return result;
}

bool parsePolygon( const wxString& text, BOARD_STRUCTURE_POLYGON& polygon )
{
    wxStringTokenizer tokenizer( text, ";" );
    bool malformed = false;
    while( tokenizer.HasMoreTokens() )
    {
        wxString token = tokenizer.GetNextToken();
        double x = 0.0, y = 0.0;
        char extra = 0;
        if( sscanf( token.ToUTF8().data(), " %lf , %lf %c", &x, &y, &extra ) == 2 && std::isfinite( x ) && std::isfinite( y ) )
            polygon.outline.emplace_back( KiROUND( x * 1e6 ), KiROUND( y * 1e6 ) );
        else malformed = true;
    }
    return !malformed && polygon.outline.size() >= 3;
}

bool strictDouble( const wxString& text, double& value )
{
    wxString s = text; s.Trim( true ).Trim( false ); return !s.empty() && s.ToDouble( &value ) && std::isfinite( value );
}
wxString afterWord( const wxString& text, const wxString& word )
{
    const int pos = text.Find( word ); return pos == wxNOT_FOUND ? wxString() : text.Mid( pos + word.Length() ).Trim( false );
}
}

PANEL_SETUP_RIGID_FLEX::PANEL_SETUP_RIGID_FLEX( wxWindow* aParent, PCB_EDIT_FRAME* aFrame ) :
        wxPanel( aParent ), m_frame( aFrame ), m_book( new wxNotebook( this, wxID_ANY ) ),
        m_regions( nullptr ), m_stacks( nullptr ), m_stiffeners( nullptr ),
        m_coverlays( nullptr ), m_bends( nullptr ), m_validation( nullptr ), m_preview( nullptr )
        , m_board( aFrame ? aFrame->GetBoard() : nullptr )
{
    wxBoxSizer* root = new wxBoxSizer( wxVERTICAL );
    root->Add( new wxStaticText( this, wxID_ANY,
                                 _( "Define physical regions, local stackups, reinforcement, coverlay, and bends. "
                                    "Electrical artwork remains in the flat board coordinates." ) ),
               0, wxALL | wxEXPAND, 6 );
    m_preview = new wxPanel( this, wxID_ANY, wxDefaultPosition, wxSize( -1, 130 ) );
    m_preview->SetBackgroundStyle( wxBG_STYLE_PAINT );
    m_preview->Bind( wxEVT_PAINT, &PANEL_SETUP_RIGID_FLEX::OnPreviewPaint, this );
    root->Add( m_preview, 0, wxALL | wxEXPAND, 6 );

    BuildRecordPage( m_book, _( "Regions" ), m_regions,
                     { _( "Name" ), _( "Type (derived)" ), _( "Regional stack" ), _( "Outline (mm)" ), _( "ID (read-only)" ) } );
    BuildRecordPage( m_book, _( "Regional stackup" ), m_stacks,
                     { _( "Name" ), _( "Kind" ), _( "Copper layer IDs" ), _( "Construction" ), _( "Total (mm)" ), _( "Continuity" ) } );
    BuildRecordPage( m_book, _( "Stiffeners" ), m_stiffeners,
                     { _( "ID (read-only)" ), _( "Side" ), _( "Material" ), _( "Outline / region" ), _( "Adhesive, tolerance" ) } );
    BuildRecordPage( m_book, _( "Coverlay" ), m_coverlays,
                     { _( "Name" ), _( "Side" ), _( "Material" ), _( "Coverage / openings" ), _( "Registration" ) } );
    BuildRecordPage( m_book, _( "Bends" ), m_bends,
                     { _( "Name" ), _( "Region" ), _( "Angle" ), _( "Radius" ), _( "Fold order / axis" ) } );
    root->Add( m_book, 1, wxEXPAND | wxLEFT | wxRIGHT, 6 );

    wxBoxSizer* buttons = new wxBoxSizer( wxHORIZONTAL );
    buttons->Add( new wxButton( this, ID_RF_ADD, _( "Add" ) ), 0, wxRIGHT, 4 );
    buttons->Add( new wxButton( this, ID_RF_DELETE, _( "Delete" ) ), 0, wxRIGHT, 12 );
    buttons->Add( new wxButton( this, ID_RF_LAYERS, _( "Edit stack layers…" ) ), 0, wxRIGHT, 12 );
    buttons->Add( new wxStaticText( this, wxID_ANY, _( "Validation:" ) ), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4 );
    m_validation = new wxTextCtrl( this, wxID_ANY, _( "No structural errors" ), wxDefaultPosition,
                                   wxDefaultSize, wxTE_READONLY );
    buttons->Add( m_validation, 1, wxEXPAND );
    root->Add( buttons, 0, wxALL | wxEXPAND, 6 );
    SetSizerAndFit( root );

    if( m_board )
    {
        m_stagedStructure = m_board->GetBoardStructure();
        std::vector<RIGID_FLEX_UI_RECORD> regions, stacks, stiffeners, coverlays, bends;
        const BOARD_STRUCTURE& structure = m_board->GetBoardStructure();
        for( const auto& s : structure.stacks )
        {
            wxString materials;
            wxString copperLayers;
            for( int layer : s.copper_layers )
            {
                if( !copperLayers.empty() ) copperLayers += ",";
                copperLayers += wxString::Format( "%d", layer );
            }
            materials = wxString::Format( "%zu layers", s.layers.size() );
            double thickness = 0;
            for( const auto& material : s.materials ) thickness += material.thickness_nm / 1e6;
            stacks.push_back( { wxString::FromUTF8( s.name ),
                                s.kind == BOARD_STRUCTURE_STACK_KIND::FLEX ? _( "Flex" ) : _( "Rigid" ),
                                copperLayers, materials,
                                wxString::Format( "%.4f", thickness ),
                                wxString::FromUTF8( s.continuity_id ) } );
        }
        for( const auto& r : structure.regions )
        {
            const BOARD_STRUCTURE_STACK* stack = structure.StackForRegion( r );
            regions.push_back( { wxString::FromUTF8( r.name ),
                                 stack ? ( stack->kind == BOARD_STRUCTURE_STACK_KIND::FLEX ? _( "Flex" ) : _( "Rigid" ) ) : _( "Unknown" ),
                                 wxString::FromUTF8( r.stack_id ), polygonText( r.shape ),
                                 wxString::FromUTF8( r.id ), wxEmptyString } );
        }
        for( const auto& s : structure.stiffeners )
            stiffeners.push_back( { wxString::FromUTF8( s.id ), s.top ? _( "Top" ) : _( "Bottom" ),
                                    wxString::FromUTF8( s.material ), wxString::Format( "%s | %s", wxString::FromUTF8( s.region_id ), polygonText( s.shape ) ),
                                    wxString::Format( "%s; %.3f mm [%.3f..%.3f]", wxString::FromUTF8( s.adhesive ),
                                                      s.thickness_nm / 1e6, s.min_thickness_nm / 1e6,
                                                      s.max_thickness_nm / 1e6 ) + wxString::Format( "; adhesive %.3f mm", s.adhesive_thickness_nm / 1e6 ), wxEmptyString } );
        for( const auto& c : structure.coverlays )
            coverlays.push_back( { wxString::FromUTF8( c.region_id ), c.top ? _( "Top" ) : _( "Bottom" ),
                                   wxString::FromUTF8( c.material ), wxString::Format( "%s | %s", wxString::FromUTF8( c.region_id ), polygonText( c.coverage ) ),
                                   wxString::Format( "%s; thickness %.3f mm; adhesive %.3f mm; registration %.3f mm", wxString::FromUTF8( c.adhesive ), c.thickness_nm / 1e6, c.adhesive_thickness_nm / 1e6, c.registration_tolerance_nm / 1e6 ), wxEmptyString } );
        for( const auto& b : structure.bends )
            bends.push_back( { wxString::FromUTF8( b.id ), wxString::FromUTF8( b.region_id ),
                               wxString::Format( "%.3f deg", b.angle_degrees ), wxString::Format( "%.3f mm", b.inner_radius_mm ),
                               wxString::Format( "sequence %d; axis %.4f,%.4f -> %.4f,%.4f; side %s; neutral %.4f", b.sequence,
                                                 b.start.x / 1e6, b.start.y / 1e6, b.end.x / 1e6, b.end.y / 1e6,
                                                 b.moving_side_left ? "left" : "right", b.neutral_axis_offset_mm ), wxEmptyString } );
        LoadRecords( regions, stacks, stiffeners, coverlays, bends );
    }

    Bind( wxEVT_BUTTON, &PANEL_SETUP_RIGID_FLEX::OnAdd, this, ID_RF_ADD );
    Bind( wxEVT_BUTTON, &PANEL_SETUP_RIGID_FLEX::OnDelete, this, ID_RF_DELETE );
    Bind( wxEVT_BUTTON, [this]( wxCommandEvent& ) { EditSelectedStackLayers(); }, ID_RF_LAYERS );
}

void PANEL_SETUP_RIGID_FLEX::EditSelectedStackLayers()
{
    if( m_book->GetSelection() != 1 ) return;
    const int row = m_stacks->GetGridCursorRow();
    if( row < 0 || row >= (int) m_stagedStructure.stacks.size()
        || row >= (int) m_stackData.size() ) return;
    STACK_LAYERS_DIALOG dialog( this, m_stagedStructure.stacks[row] );
    if( dialog.ShowModal() == wxID_OK )
    {
        wxString copper;
        for( const auto& layer : m_stagedStructure.stacks[row].copper_layers )
        {
            if( !copper.empty() ) copper += ",";
            copper += wxString::Format( "%d", layer );
        }
        m_stackData[row].stack = copper;
        if( !m_stagedStructure.stacks[row].materials.empty() )
        {
            double thickness = 0;
            for( const auto& material : m_stagedStructure.stacks[row].materials )
                thickness += material.thickness_nm / 1e6;
            m_stackData[row].geometry = wxString::Format( "%zu layers", m_stagedStructure.stacks[row].layers.size() );
            m_stackData[row].details = wxString::Format( "%.4f", thickness );
        }
        m_stacks->SetCellValue( row, 2, copper );
        m_stacks->SetCellValue( row, 3, m_stackData[row].geometry );
        m_stacks->SetCellValue( row, 4, m_stackData[row].details );
        MarkChanged();
    }
}

void PANEL_SETUP_RIGID_FLEX::BuildRecordPage( wxNotebook* aBook, const wxString& aTitle,
                                               wxGrid*& aGrid, const wxArrayString& aColumns )
{
    wxPanel* page = new wxPanel( aBook );
    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );
    aGrid = new wxGrid( page, wxID_ANY );
    aGrid->CreateGrid( 0, aColumns.size() );
    aGrid->SetRowLabelSize( 32 );
    const int widths[] =
    {
        170, 170, 170, 170, 170, 170
    };

    if( aTitle == _( "Regions" ) )
    {
        const int regionWidths[] = { 170, 125, 150, 430, 180 };
        for( unsigned i = 0; i < aColumns.size(); ++i )
            aGrid->SetColSize( i, regionWidths[i] );
    }
    else if( aTitle == _( "Regional stackup" ) )
    {
        const int stackWidths[] = { 170, 100, 170, 180, 120, 180 };
        for( unsigned i = 0; i < aColumns.size(); ++i )
            aGrid->SetColSize( i, stackWidths[i] );
    }
    else if( aTitle == _( "Stiffeners" ) || aTitle == _( "Coverlay" ) )
    {
        const int overlayWidths[] = { 170, 100, 140, 360, 350 };
        for( unsigned i = 0; i < aColumns.size(); ++i )
            aGrid->SetColSize( i, overlayWidths[i] );
    }
    else if( aTitle == _( "Bends" ) )
    {
        const int bendWidths[] = { 170, 170, 120, 120, 470 };
        for( unsigned i = 0; i < aColumns.size(); ++i )
            aGrid->SetColSize( i, bendWidths[i] );
    }
    else
    {
        for( unsigned i = 0; i < aColumns.size(); ++i )
            aGrid->SetColSize( i, widths[i] );
    }
    for( unsigned i = 0; i < aColumns.size(); ++i )
        aGrid->SetColLabelValue( i, aColumns[i] );
    aGrid->Bind( wxEVT_GRID_CELL_CHANGED, &PANEL_SETUP_RIGID_FLEX::OnCellChanged, this );
    sizer->Add( aGrid, 1, wxEXPAND | wxALL, 2 );
    page->SetSizer( sizer );
    aBook->AddPage( page, aTitle );
}

wxGrid* PANEL_SETUP_RIGID_FLEX::CurrentGrid() const { return m_book->GetSelection() == 0 ? m_regions :
    m_book->GetSelection() == 1 ? m_stacks : m_book->GetSelection() == 2 ? m_stiffeners :
    m_book->GetSelection() == 3 ? m_coverlays : m_bends; }

std::vector<RIGID_FLEX_UI_RECORD>& PANEL_SETUP_RIGID_FLEX::CurrentRecords()
{
    return m_book->GetSelection() == 0 ? m_regionData : m_book->GetSelection() == 1 ? m_stackData :
           m_book->GetSelection() == 2 ? m_stiffenerData : m_book->GetSelection() == 3 ? m_coverlayData : m_bendData;
}
const std::vector<RIGID_FLEX_UI_RECORD>& PANEL_SETUP_RIGID_FLEX::CurrentRecords() const
{ return const_cast<PANEL_SETUP_RIGID_FLEX*>( this )->CurrentRecords(); }

void PANEL_SETUP_RIGID_FLEX::OnAdd( wxCommandEvent& )
{
    wxGrid* grid = CurrentGrid();
    auto& records = CurrentRecords();

    // Regional stacks must always be physical stacks.  Use the starter-stack
    // chooser so a new row cannot enter the staged model with missing copper
    // mappings or layer instances.
    if( m_book->GetSelection() == 1 )
    {
        const wxString choices[] = { _( "Rigid" ), _( "Flex" ) };
        wxSingleChoiceDialog kindDialog( this, _( "Choose the regional stack type:" ),
                                         _( "Add Regional Stack" ), 2, choices );
        if( kindDialog.ShowModal() != wxID_OK )
            return;

        const BOARD_STRUCTURE_STACK_KIND kind = kindDialog.GetSelection() == 1
                                                        ? BOARD_STRUCTURE_STACK_KIND::FLEX
                                                        : BOARD_STRUCTURE_STACK_KIND::RIGID;
        std::string stackId;
        if( !ChooseStructureStack( m_frame, m_stagedStructure, kind, stackId, true ) )
            return;

        const BOARD_STRUCTURE_STACK* chosen = nullptr;
        for( const auto& stack : m_stagedStructure.stacks )
            if( stack.id == stackId )
            {
                chosen = &stack;
                break;
            }
        if( !chosen )
            return;

        RIGID_FLEX_UI_RECORD record;
        record.name = wxString::FromUTF8( chosen->name );
        record.type = chosen->kind == BOARD_STRUCTURE_STACK_KIND::FLEX ? _( "Flex" ) : _( "Rigid" );
        for( int layer : chosen->copper_layers )
        {
            if( !record.stack.empty() )
                record.stack += ",";
            record.stack += wxString::Format( "%d", layer );
        }
        for( const auto& material : chosen->materials )
        {
            if( !record.geometry.empty() )
                record.geometry += "; ";
            record.geometry += wxString::FromUTF8( material.material );
        }
        double totalMm = 0.0;
        for( const auto& material : chosen->materials )
            totalMm += material.thickness_nm / 1e6;
        record.details = wxString::Format( "%.4f", totalMm );
        record.extra = wxString::FromUTF8( chosen->continuity_id );
        records.push_back( record );
        grid->AppendRows();
        const int newRow = grid->GetNumberRows() - 1;
        const wxString* values[] = { &records.back().name, &records.back().type, &records.back().stack,
                                     &records.back().geometry, &records.back().details, &records.back().extra };
        for( int col = 0; col < grid->GetNumberCols() && col < 6; ++col )
            grid->SetCellValue( newRow, col, *values[col] );
        MarkChanged();
        return;
    }

    records.push_back( { wxString::Format( _( "Unnamed %zu" ), records.size() + 1 ), wxEmptyString,
                         wxEmptyString, wxEmptyString, wxEmptyString, wxEmptyString } );
    const std::string id = wxString::Format( "rf-%d-%zu", m_book->GetSelection(), records.size() ).ToUTF8().data();
    if( m_book->GetSelection() == 0 )
    {
        BOARD_STRUCTURE_REGION r; r.id = id; r.name = records.back().name.ToUTF8().data();
        r.shape.outline = { VECTOR2I( 0, 0 ), VECTOR2I( 1000000, 0 ), VECTOR2I( 1000000, 1000000 ) };
        m_stagedStructure.regions.push_back( r );
        records.back().type = _( "Rigid" );
        records.back().geometry = polygonText( r.shape );
        records.back().details = wxString::FromUTF8( id );
    }
    else if( m_book->GetSelection() == 2 )
    {
        BOARD_STRUCTURE_STIFFENER s; s.id = id; s.material = "FR4"; s.adhesive = "Acrylic";
        s.thickness_nm = 100000; s.min_thickness_nm = 100000; s.max_thickness_nm = 100000;
        s.shape.outline = { VECTOR2I( 0, 0 ), VECTOR2I( 100000, 0 ), VECTOR2I( 100000, 100000 ) };
        if( !m_stagedStructure.regions.empty() )
        {
            s.region_id = m_stagedStructure.regions.front().id;
            s.shape = m_stagedStructure.regions.front().shape;
        }
        m_stagedStructure.stiffeners.push_back( s );
        records.back().type = _( "Top" );
        records.back().stack = _( "FR4" );
        records.back().geometry = wxString::FromUTF8( s.region_id ) + " | " + polygonText( s.shape );
        records.back().details = _( "Acrylic; 0.100 mm [0.100..0.100]; adhesive 0.000 mm" );
    }
    else if( m_book->GetSelection() == 3 )
    {
        BOARD_STRUCTURE_COVERLAY c; c.id = id; c.material = "Polyimide"; c.thickness_nm = 25000;
        c.coverage.outline = { VECTOR2I( 0, 0 ), VECTOR2I( 100000, 0 ), VECTOR2I( 100000, 100000 ) };
        if( !m_stagedStructure.regions.empty() ) c.region_id = m_stagedStructure.regions.front().id;
        c.adhesive = "Acrylic"; c.adhesive_thickness_nm = 25000; c.registration_tolerance_nm = 75000;
        m_stagedStructure.coverlays.push_back( c );
        records.back().type = _( "Top" );
        records.back().stack = _( "Polyimide" );
        records.back().geometry = wxString::FromUTF8( c.region_id ) + " | " + polygonText( c.coverage );
        records.back().details = _( "Acrylic; thickness 0.025 mm; adhesive 0.025 mm; registration 0.075 mm" );
    }
    else
    {
        BOARD_STRUCTURE_BEND b; b.id = id; b.start = VECTOR2I( 0, 0 ); b.end = VECTOR2I( 1000000, 0 );
        b.inner_radius_mm = 1.0;
        for( const auto& region : m_stagedStructure.regions )
            if( const auto* stack = m_stagedStructure.StackForRegion( region ); stack
                && stack->kind == BOARD_STRUCTURE_STACK_KIND::FLEX )
            { b.region_id = region.id; break; }
        m_stagedStructure.bends.push_back( b );
        records.back().type = wxString::FromUTF8( b.region_id );
        records.back().stack = _( "90.000 deg" );
        records.back().geometry = _( "1.000 mm" );
        records.back().details = _( "sequence 0; axis 0.0000,0.0000 -> 1.0000,0.0000; side left; neutral -1.0000" );
    }
    grid->AppendRows();
    const int newRow = grid->GetNumberRows() - 1;
    const wxString* values[] = { &records.back().name, &records.back().type, &records.back().stack,
                                 &records.back().geometry, &records.back().details, &records.back().extra };
    for( int col = 0; col < grid->GetNumberCols() && col < 6; ++col )
        grid->SetCellValue( newRow, col, *values[col] );
    if( m_book->GetSelection() == 0 )
    {
        grid->SetReadOnly( newRow, 1 );
        grid->SetReadOnly( newRow, 4 );
    }
    else if( m_book->GetSelection() == 2 )
    {
        // Stiffeners use their generated ID as the stable model key; there is
        // no separate name field to persist in BOARD_STRUCTURE_STIFFENER.
        grid->SetReadOnly( newRow, 0 );
    }
    MarkChanged();
}

void PANEL_SETUP_RIGID_FLEX::OnDelete( wxCommandEvent& )
{
    wxGrid* grid = CurrentGrid();
    int row = grid->GetGridCursorRow();
    if( row < 0 || row >= (int) CurrentRecords().size() ) return;
    if( m_book->GetSelection() == 0 && row < (int) m_stagedStructure.regions.size() )
        m_stagedStructure.regions.erase( m_stagedStructure.regions.begin() + row );
    else if( m_book->GetSelection() == 1 && row < (int) m_stagedStructure.stacks.size() )
        m_stagedStructure.stacks.erase( m_stagedStructure.stacks.begin() + row );
    else if( m_book->GetSelection() == 2 && row < (int) m_stagedStructure.stiffeners.size() )
        m_stagedStructure.stiffeners.erase( m_stagedStructure.stiffeners.begin() + row );
    else if( m_book->GetSelection() == 3 && row < (int) m_stagedStructure.coverlays.size() )
        m_stagedStructure.coverlays.erase( m_stagedStructure.coverlays.begin() + row );
    else if( row < (int) m_stagedStructure.bends.size() )
        m_stagedStructure.bends.erase( m_stagedStructure.bends.begin() + row );
    CurrentRecords().erase( CurrentRecords().begin() + row );
    grid->DeleteRows( row );
    MarkChanged();
}

void PANEL_SETUP_RIGID_FLEX::OnCellChanged( wxGridEvent& aEvent )
{
    auto& record = CurrentRecords().at( aEvent.GetRow() );
    wxString* fields[] = { &record.name, &record.type, &record.stack, &record.geometry, &record.details, &record.extra };
    if( aEvent.GetCol() < 6 ) *fields[aEvent.GetCol()] = CurrentGrid()->GetCellValue( aEvent.GetRow(), aEvent.GetCol() );
    // Keep the noncommitting preview synchronized with geometry edits.
    if( m_book->GetSelection() == 0 && aEvent.GetCol() == 3 && aEvent.GetRow() < (int) m_stagedStructure.regions.size() )
    { BOARD_STRUCTURE_POLYGON p; if( parsePolygon( record.geometry, p ) ) m_stagedStructure.regions[aEvent.GetRow()].shape.outline = p.outline; }
    if( m_book->GetSelection() == 2 && aEvent.GetCol() == 3 && aEvent.GetRow() < (int) m_stagedStructure.stiffeners.size() )
    { BOARD_STRUCTURE_POLYGON p; if( parsePolygon( record.geometry.AfterFirst( '|' ), p ) ) m_stagedStructure.stiffeners[aEvent.GetRow()].shape.outline = p.outline; }
    if( m_book->GetSelection() == 3 && aEvent.GetCol() == 3 && aEvent.GetRow() < (int) m_stagedStructure.coverlays.size() )
    { BOARD_STRUCTURE_POLYGON p; if( parsePolygon( record.geometry.AfterFirst( '|' ), p ) ) m_stagedStructure.coverlays[aEvent.GetRow()].coverage.outline = p.outline; }
    if( m_book->GetSelection() == 4 && aEvent.GetCol() == 4 && aEvent.GetRow() < (int) m_stagedStructure.bends.size() )
    { double x1=0,y1=0,x2=0,y2=0; if( sscanf( record.details.ToUTF8().data(), "sequence %*d; axis %lf,%lf -> %lf,%lf", &x1,&y1,&x2,&y2 ) == 4 ) { m_stagedStructure.bends[aEvent.GetRow()].start = VECTOR2I( KiROUND(x1*1e6),KiROUND(y1*1e6) ); m_stagedStructure.bends[aEvent.GetRow()].end = VECTOR2I( KiROUND(x2*1e6),KiROUND(y2*1e6) ); } }
    MarkChanged();
    aEvent.Skip();
}

void PANEL_SETUP_RIGID_FLEX::MarkChanged()
{
    wxString error;
    Validate( &error );
    m_validation->SetValue( error.empty() ? _( "No structural errors" ) : error );
    m_preview->Refresh();
}

void PANEL_SETUP_RIGID_FLEX::OnPreviewPaint( wxPaintEvent& )
{
    wxAutoBufferedPaintDC dc( m_preview );
    dc.SetBackground( wxBrush( m_preview->GetBackgroundColour() ) );
    dc.Clear();
    int minX = 0, minY = 0, maxX = 1, maxY = 1;
    bool havePoint = false;
    auto visit = [&]( const VECTOR2I& p )
    {
        if( !havePoint ) minX = maxX = p.x, minY = maxY = p.y, havePoint = true;
        else minX = std::min( minX, p.x ), minY = std::min( minY, p.y ),
             maxX = std::max( maxX, p.x ), maxY = std::max( maxY, p.y );
    };
    auto visitPoly = [&]( const BOARD_STRUCTURE_POLYGON& shape )
    { for( const auto& p : shape.outline ) visit( p ); for( const auto& hole : shape.holes ) for( const auto& p : hole ) visit( p ); };
    for( const auto& r : m_stagedStructure.regions ) visitPoly( r.shape );
    for( const auto& s : m_stagedStructure.stiffeners ) visitPoly( s.shape );
    for( const auto& c : m_stagedStructure.coverlays ) visitPoly( c.coverage );
    for( const auto& b : m_stagedStructure.bends ) visit( b.start ), visit( b.end );
    const wxSize size = m_preview->GetClientSize();
    const double sx = ( size.x - 40.0 ) / std::max( 1, maxX - minX );
    const double sy = ( size.y - 65.0 ) / std::max( 1, maxY - minY );
    const double scale = std::max( 1e-12, std::min( sx, sy ) );
    auto map = [&]( const VECTOR2I& p )
    { return wxPoint( 20 + KiROUND( ( p.x - minX ) * scale ),
                      20 + KiROUND( ( p.y - minY ) * scale ) ); };
    auto drawPoly = [&]( const BOARD_STRUCTURE_POLYGON& shape )
    {
        auto contour = [&]( const std::vector<VECTOR2I>& points )
        {
            if( points.size() < 3 ) return;
            wxPoint last = map( points.front() );
            for( size_t i = 1; i <= points.size(); ++i )
            {
                wxPoint next = map( points[i % points.size()] );
                dc.DrawLine( last, next );
                last = next;
            }
        };
        contour( shape.outline );
        for( const auto& hole : shape.holes ) contour( hole );
    };
    dc.SetPen( wxPen( wxColour( 55, 110, 170 ), 2 ) );
    dc.SetBrush( *wxTRANSPARENT_BRUSH );
    for( const auto& region : m_stagedStructure.regions ) drawPoly( region.shape );
    dc.SetPen( wxPen( wxColour( 100, 160, 100 ), 2, wxPENSTYLE_SHORT_DASH ) );
    for( const auto& stiffener : m_stagedStructure.stiffeners ) drawPoly( stiffener.shape );
    dc.SetPen( wxPen( wxColour( 150, 100, 190 ), 2, wxPENSTYLE_DOT ) );
    for( const auto& coverlay : m_stagedStructure.coverlays )
    {
        drawPoly( coverlay.coverage );
        for( const auto& opening : coverlay.openings ) drawPoly( opening );
    }
    dc.SetPen( wxPen( wxColour( 210, 100, 45 ), 2, wxPENSTYLE_DOT ) );
    for( const auto& bend : m_stagedStructure.bends )
    {
        const wxPoint a = map( bend.start ), b = map( bend.end );
        dc.DrawLine( a, b );
        double thickness = 0.0;
        for( const auto& region : m_stagedStructure.regions )
            if( region.id == bend.region_id )
                if( const auto* stack = m_stagedStructure.StackForRegion( region ) )
                    for( const auto& material : stack->materials )
                        thickness += material.thickness_nm * 1e-6;
        const double dx = double( bend.end.x ) - bend.start.x;
        const double dy = double( bend.end.y ) - bend.start.y;
        const double length = std::hypot( dx, dy );
        if( length > 0.0 )
        {
            const double width = BOARD_STRUCTURE::BendStripLengthMm( bend, thickness )
                                 * 1e6 * scale * ( bend.moving_side_left ? 1.0 : -1.0 );
            const wxPoint normal( KiROUND( -dy * width / length ),
                                  KiROUND( dx * width / length ) );
            dc.DrawLine( a + normal, b + normal );
            dc.DrawLine( a, a + normal );
            dc.DrawLine( b, b + normal );
        }
    }
    dc.SetTextForeground( wxColour( 190, 190, 190 ) );
    dc.DrawText( _( "Blue: regions   Orange: developed bend strips   Green: stiffeners   Purple: coverlay" ), 20, size.y - 25 );
}

bool PANEL_SETUP_RIGID_FLEX::Validate( wxString* aError ) const
{
    for( const auto& region : m_regionData )
    {
        BOARD_STRUCTURE_POLYGON polygon;
        if( region.name.empty() || region.stack.empty() || !parsePolygon( region.geometry, polygon ) )
        { if( aError ) *aError = _( "Each region needs a name, stack, and valid polygon." ); return false; }
    }
    for( const auto& stack : m_stackData )
    {
        double thickness = 0;
        if( stack.name.empty() || !strictDouble( stack.details, thickness ) || thickness <= 0 )
        { if( aError ) *aError = _( "Each stack needs a name and positive thickness." ); return false; }
        wxStringTokenizer copper( stack.stack, "," ); long layer = 0;
        while( copper.HasMoreTokens() ) if( !copper.GetNextToken().ToLong( &layer ) || layer < 0 || layer >= PCB_LAYER_ID_COUNT || !IsCopperLayer( layer ) )
        { if( aError ) *aError = _( "Stack copper layers must be valid numeric copper IDs." ); return false; }
    }
    for( const auto& stiffener : m_stiffenerData )
    {
        double thickness = 0; if( !strictDouble( stiffener.details.AfterFirst( ';' ).Trim( false ).BeforeFirst( ' ' ), thickness ) || thickness <= 0 )
        { if( aError ) *aError = _( "Stiffener thickness must be positive and numeric." ); return false; }
    }
    for( const auto& coverlay : m_coverlayData )
    {
        double registration = 0;
        if( !strictDouble( afterWord( coverlay.details, "registration" ).BeforeFirst( ' ' ), registration ) || registration < 0 )
        { if( aError ) *aError = _( "Coverlay registration tolerance must be numeric." ); return false; }
    }
    for( const auto& bend : m_bendData )
    {
        if( bend.name.empty() ) { if( aError ) *aError = _( "Bend needs a name" ); return false; }
        double angle = 0, radius = 0;
        if( !strictDouble( bend.stack.BeforeFirst( ' ' ), angle ) || !strictDouble( bend.geometry.BeforeFirst( ' ' ), radius ) || radius <= 0 )
        { if( aError ) *aError = _( "Bend angle and radius must be numeric; radius must be positive." ); return false; }
    }
    if( aError ) aError->clear();
    return true;
}

bool PANEL_SETUP_RIGID_FLEX::TransferDataFromWindow()
{
    auto syncGrid = []( wxGrid* grid, std::vector<RIGID_FLEX_UI_RECORD>& records )
    {
        if( grid->IsCellEditControlShown() )
            grid->SaveEditControlValue();

        for( int row = 0; row < grid->GetNumberRows() && row < (int) records.size(); ++row )
        {
            wxString* fields[] = { &records[row].name, &records[row].type, &records[row].stack,
                                   &records[row].geometry, &records[row].details, &records[row].extra };
            for( int col = 0; col < grid->GetNumberCols() && col < 6; ++col )
                *fields[col] = grid->GetCellValue( row, col );
        }
    };

    syncGrid( m_regions, m_regionData );
    syncGrid( m_stacks, m_stackData );
    syncGrid( m_stiffeners, m_stiffenerData );
    syncGrid( m_coverlays, m_coverlayData );
    syncGrid( m_bends, m_bendData );

    wxString error;
    if( !Validate( &error ) ) { m_validation->SetValue( error ); return false; }
    if( !m_board ) return true;
    BOARD_STRUCTURE& structure = m_stagedStructure;
    // The native model retains geometry and physical values.  This page edits
    // the human-facing names/links and leaves geometry-authoritative fields to
    // the structure tools, avoiding accidental loss of polygon data.
    for( size_t i = 0; i < m_stackData.size() && i < structure.stacks.size(); ++i )
    {
        structure.stacks[i].name = m_stackData[i].name.ToUTF8().data();
        structure.stacks[i].continuity_id = m_stackData[i].extra.ToUTF8().data();
        structure.stacks[i].kind = m_stackData[i].type.CmpNoCase( _( "Flex" ) ) == 0
                                           ? BOARD_STRUCTURE_STACK_KIND::FLEX
                                           : BOARD_STRUCTURE_STACK_KIND::RIGID;
        // A stack row is intentionally a compact editor: the material column
        // accepts a semicolon-separated material list, preserving existing
        // material thicknesses when no numeric replacement was entered.
        structure.stacks[i].copper_layers.clear();
        wxStringTokenizer copper( m_stackData[i].stack, "," );
        while( copper.HasMoreTokens() )
        {
            long layer = -1;
            if( copper.GetNextToken().ToLong( &layer ) && layer >= 0 ) structure.stacks[i].copper_layers.push_back( layer );
        }
    }
    for( size_t i = 0; i < m_regionData.size() && i < structure.regions.size(); ++i )
    {
        structure.regions[i].name = m_regionData[i].name.ToUTF8().data();
        structure.regions[i].stack_id = m_regionData[i].stack.ToUTF8().data();
        BOARD_STRUCTURE_POLYGON polygon;
        if( parsePolygon( m_regionData[i].geometry, polygon ) ) structure.regions[i].shape.outline = polygon.outline;
    }
    for( size_t i = 0; i < m_stiffenerData.size() && i < structure.stiffeners.size(); ++i )
    {
        auto& item = structure.stiffeners[i];
        wxString geometry = m_stiffenerData[i].geometry;
        item.region_id = geometry.BeforeFirst( '|' ).Trim().ToUTF8().data();
        BOARD_STRUCTURE_POLYGON polygon;
        if( parsePolygon( geometry.AfterFirst( '|' ), polygon ) ) item.shape.outline = polygon.outline;
        item.material = m_stiffenerData[i].stack.ToUTF8().data();
        item.top = m_stiffenerData[i].type.CmpNoCase( _( "Bottom" ) ) != 0;
        wxString stiffDetails = m_stiffenerData[i].details;
        item.adhesive = stiffDetails.BeforeFirst( ';' ).Trim().ToUTF8().data();
        double t = 0, minT = 0, maxT = 0;
        strictDouble( stiffDetails.AfterFirst( ';' ).Trim( false ).BeforeFirst( ' ' ), t );
        wxString range = stiffDetails.AfterFirst( '[' ).BeforeLast( ']' );
        if( !range.empty() && sscanf( range.ToUTF8().data(), "%lf..%lf", &minT, &maxT ) != 2 ) return false;
        item.thickness_nm = KiROUND( t * 1e6 ); item.min_thickness_nm = KiROUND( minT * 1e6 ); item.max_thickness_nm = KiROUND( maxT * 1e6 );
        double adhesiveThickness = 0; strictDouble( afterWord( stiffDetails, "adhesive" ).BeforeFirst( ' ' ), adhesiveThickness );
        item.adhesive_thickness_nm = KiROUND( adhesiveThickness * 1e6 );
    }
    for( size_t i = 0; i < m_coverlayData.size() && i < structure.coverlays.size(); ++i )
    {
        auto& item = structure.coverlays[i];
        wxString geometry = m_coverlayData[i].geometry;
        item.region_id = geometry.BeforeFirst( '|' ).Trim().ToUTF8().data();
        BOARD_STRUCTURE_POLYGON polygon;
        if( parsePolygon( geometry.AfterFirst( '|' ), polygon ) ) item.coverage.outline = polygon.outline;
        item.material = m_coverlayData[i].stack.ToUTF8().data();
        item.top = m_coverlayData[i].type.CmpNoCase( _( "Bottom" ) ) != 0;
        wxString coverDetails = m_coverlayData[i].details;
        item.adhesive = coverDetails.BeforeFirst( ';' ).Trim().ToUTF8().data();
        double registration = 0; strictDouble( afterWord( coverDetails, "registration" ).BeforeFirst( ' ' ), registration );
        item.registration_tolerance_nm = KiROUND( registration * 1e6 );
        double coverThickness = 0, coverAdhesive = 0;
        strictDouble( afterWord( coverDetails, "thickness" ).BeforeFirst( ' ' ), coverThickness );
        strictDouble( afterWord( coverDetails, "adhesive" ).BeforeFirst( ' ' ), coverAdhesive );
        item.thickness_nm = KiROUND( coverThickness * 1e6 ); item.adhesive_thickness_nm = KiROUND( coverAdhesive * 1e6 );
    }
    for( size_t i = 0; i < m_bendData.size() && i < structure.bends.size(); ++i )
    {
        structure.bends[i].region_id = m_bendData[i].type.ToUTF8().data();
        double angle = 0, radius = 0;
        if( sscanf( m_bendData[i].stack.ToUTF8().data(), "%lf", &angle ) == 1 ) structure.bends[i].angle_degrees = angle;
        if( sscanf( m_bendData[i].geometry.ToUTF8().data(), "%lf", &radius ) == 1 ) structure.bends[i].inner_radius_mm = radius;
        double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        int sequence = 0; char side[16] = {}; double neutral = -1;
        if( sscanf( m_bendData[i].details.ToUTF8().data(), "sequence %d; axis %lf,%lf -> %lf,%lf; side %15[^;]; neutral %lf", &sequence, &x1, &y1, &x2, &y2, side, &neutral ) >= 5 )
        {
            structure.bends[i].sequence = sequence;
            if( side[0] ) structure.bends[i].moving_side_left = strcmp( side, "right" ) != 0;
            if( neutral >= 0 ) structure.bends[i].neutral_axis_offset_mm = neutral;
            structure.bends[i].start = VECTOR2I( KiROUND( x1 * 1e6 ), KiROUND( y1 * 1e6 ) );
            structure.bends[i].end = VECTOR2I( KiROUND( x2 * 1e6 ), KiROUND( y2 * 1e6 ) );
        }
    }
    const BOARD_STRUCTURE_VALIDATION structuralValidation = structure.Validate();
    if( !structuralValidation.IsValid() )
    {
        wxString message;
        for( const std::string& item : structuralValidation.errors )
        {
            if( !message.empty() ) message += "\n";
            message += wxString::FromUTF8( item );
        }
        m_validation->SetValue( message );
        return false;
    }
    return true;
}


bool PANEL_SETUP_RIGID_FLEX::ApplyChanges()
{
    if( !TransferDataFromWindow() )
        return false;

    if( !m_board )
        return true;

    if( m_board->GetBoardStructure().ToJson() != m_stagedStructure.ToJson() )
    {
        if( m_frame )
            m_frame->CommitBoardStructure( m_stagedStructure, _( "Edit board structure" ) );
        else
            m_board->GetBoardStructure() = m_stagedStructure;
    }

    return true;
}

static void loadGrid( wxGrid* grid, const std::vector<RIGID_FLEX_UI_RECORD>& records )
{
    if( grid->GetNumberRows() ) grid->DeleteRows( 0, grid->GetNumberRows() );
    grid->AppendRows( records.size() );
    for( size_t row = 0; row < records.size(); ++row )
    {
        const auto& r = records[row];
        grid->SetCellValue( row, 0, r.name ); grid->SetCellValue( row, 1, r.type );
        grid->SetCellValue( row, 2, r.stack ); grid->SetCellValue( row, 3, r.geometry );
        grid->SetCellValue( row, 4, r.details );
        if( grid->GetNumberCols() > 5 ) grid->SetCellValue( row, 5, r.extra );
    }
}

void PANEL_SETUP_RIGID_FLEX::LoadRecords( const std::vector<RIGID_FLEX_UI_RECORD>& aRegions,
                                          const std::vector<RIGID_FLEX_UI_RECORD>& aStacks,
                                          const std::vector<RIGID_FLEX_UI_RECORD>& aStiffeners,
                                          const std::vector<RIGID_FLEX_UI_RECORD>& aCoverlays,
                                          const std::vector<RIGID_FLEX_UI_RECORD>& aBends )
{
    m_regionData = aRegions; m_stackData = aStacks; m_stiffenerData = aStiffeners;
    m_coverlayData = aCoverlays; m_bendData = aBends;
    loadGrid( m_regions, m_regionData ); loadGrid( m_stacks, m_stackData );
    for( int row = 0; row < m_stacks->GetNumberRows(); ++row )
    {
        m_stacks->SetReadOnly( row, 2 );
        m_stacks->SetReadOnly( row, 3 );
        m_stacks->SetReadOnly( row, 4 );
    }
    loadGrid( m_stiffeners, m_stiffenerData ); loadGrid( m_coverlays, m_coverlayData ); loadGrid( m_bends, m_bendData );
    for( int row = 0; row < m_stiffeners->GetNumberRows(); ++row )
        m_stiffeners->SetReadOnly( row, 0 );
    for( int row = 0; row < m_regions->GetNumberRows(); ++row )
    {
        m_regions->SetReadOnly( row, 1 );
        m_regions->SetReadOnly( row, 4 );
    }
}

void PANEL_SETUP_RIGID_FLEX::GetRecords( std::vector<RIGID_FLEX_UI_RECORD>& aRegions,
                                         std::vector<RIGID_FLEX_UI_RECORD>& aStacks,
                                         std::vector<RIGID_FLEX_UI_RECORD>& aStiffeners,
                                         std::vector<RIGID_FLEX_UI_RECORD>& aCoverlays,
                                         std::vector<RIGID_FLEX_UI_RECORD>& aBends ) const
{ aRegions = m_regionData; aStacks = m_stackData; aStiffeners = m_stiffenerData; aCoverlays = m_coverlayData; aBends = m_bendData; }
