#include "dialog_assembly.h"
#include "assembly_exporter.h"
#include "assembly_board_ops.h"
#include <tools/board_editor_control.h>

#include <board.h>
#include <footprint.h>
#include <STEPControl_Reader.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <chrono>
#include <pcb_edit_frame.h>
#include <pcb_base_frame.h>
#include <pcb_io/pcb_io_mgr.h>
#include <exporters/step/exporter_step.h>
#include <wx/button.h>
#include <wx/bookctrl.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/grid.h>
#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/stattext.h>
#include <wx/tokenzr.h>
#include <wx/msgdlg.h>
#include <utility>
#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <charconv>

wxBEGIN_EVENT_TABLE( DIALOG_ASSEMBLY, wxDialog )
    EVT_BUTTON( wxID_ADD, DIALOG_ASSEMBLY::OnAdd )
    EVT_BUTTON( wxID_EDIT, DIALOG_ASSEMBLY::OnEdit )
    EVT_BUTTON( wxID_DELETE, DIALOG_ASSEMBLY::OnRemove )
    EVT_BUTTON( wxID_SAVE, DIALOG_ASSEMBLY::OnSave )
    EVT_BUTTON( wxID_OPEN, DIALOG_ASSEMBLY::OnOpen )
    EVT_BUTTON( wxID_FORWARD, DIALOG_ASSEMBLY::OnExport )
    EVT_BUTTON( wxID_REFRESH, DIALOG_ASSEMBLY::OnCheck )
    EVT_BUTTON( wxID_HIGHEST + 101, DIALOG_ASSEMBLY::OnAddMate )
    EVT_BUTTON( wxID_HIGHEST + 102, DIALOG_ASSEMBLY::OnRemoveMate )
    EVT_BUTTON( wxID_HIGHEST + 103, DIALOG_ASSEMBLY::OnView3D )
    EVT_BUTTON( wxID_HIGHEST + 104, DIALOG_ASSEMBLY::OnElectricalSync )
    EVT_BUTTON( wxID_HIGHEST + 105, DIALOG_ASSEMBLY::OnAddEndpoint )
    EVT_BUTTON( wxID_HIGHEST + 106, DIALOG_ASSEMBLY::OnRemoveEndpoint )
    EVT_BUTTON( wxID_CANCEL, DIALOG_ASSEMBLY::OnCancel )
    EVT_GRID_CELL_CHANGED( DIALOG_ASSEMBLY::OnGridChanged )
    EVT_NOTEBOOK_PAGE_CHANGED( wxID_ANY, DIALOG_ASSEMBLY::OnTabChanged )
    EVT_CLOSE( DIALOG_ASSEMBLY::OnCloseWindow )
wxEND_EVENT_TABLE()

int BOARD_EDITOR_CONTROL::EditAssembly( const TOOL_EVENT& )
{
    DIALOG_ASSEMBLY::FindOrCreate( m_frame );
    return 0;
}

DIALOG_ASSEMBLY::DIALOG_ASSEMBLY( wxWindow* parent, ASSEMBLY_MANIFEST manifest ) :
        wxDialog( parent, wxID_ANY, _( "Assembly" ), wxDefaultPosition, wxSize( 1400, 950 ), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ),
        m_manifest( std::move( manifest ) )
{
    auto* top = new wxBoxSizer( wxVERTICAL );
    m_tabs = new wxNotebook( this, wxID_ANY );
    m_tabs->AddPage( new wxPanel( m_tabs ), _( "Assembly 1" ) );
    top->Add( m_tabs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8 );
    auto* identity = new wxBoxSizer( wxHORIZONTAL );
    identity->Add( new wxStaticText( this, wxID_ANY, _( "Assembly name" ) ), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8 );
    m_name = new wxTextCtrl( this, wxID_ANY );
    identity->Add( m_name, 1, wxRIGHT, 16 );
    identity->Add( new wxStaticText( this, wxID_ANY, _( "Fixed instance ID" ) ), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8 );
    m_anchor = new wxTextCtrl( this, wxID_ANY );
    identity->Add( m_anchor, 1 );
    for( wxTextCtrl* field : { m_name, m_anchor } )
        field->Bind( wxEVT_TEXT, [this]( wxCommandEvent& event ) { m_dirty = true; event.Skip(); } );
    top->Add( identity, 0, wxEXPAND | wxALL, 8 );
    m_instances = new wxGrid( this, wxID_ANY );
    m_instances->CreateGrid( 0, 10 );
    m_instances->SetColFormatBool( 9 );
    const char* labels[] = { "ID", "Kind", "Source", "Tx", "Ty", "Tz", "Rx", "Ry", "Rz", "Folded" };
    for( int c = 0; c < 10; ++c ) m_instances->SetColLabelValue( c, labels[c] );
    top->Add( m_instances, 1, wxEXPAND | wxALL, 8 );
    auto* mateLabel = new wxStaticText( this, wxID_ANY, _( "Mates (point and axis)" ) );
    top->Add( mateLabel, 0, wxLEFT | wxRIGHT, 8 );
    m_mates = new wxGrid( this, wxID_ANY );
    m_mates->CreateGrid( 0, 11 );
    m_mates->SetColFormatBool( 9 );
    const char* mateLabels[] = { "ID", "Type", "First", "Second", "First XYZ", "Second XYZ", "Clearance", "Axis A", "Axis B", "Opposing", "Status" };
    for( int c = 0; c < 11; ++c ) m_mates->SetColLabelValue( c, mateLabels[c] );
    m_mates->SetDefaultRowSize( 22 );
    top->Add( m_mates, 0, wxEXPAND | wxLEFT | wxRIGHT, 8 );
    auto* componentLabel = new wxStaticText( this, wxID_ANY, _( "Components (source board)" ) );
    top->Add( componentLabel, 0, wxLEFT | wxRIGHT, 8 );
    m_components = new wxGrid( this, wxID_ANY );
    m_components->CreateGrid( 0, 5 );
    m_components->EnableEditing( false );
    const char* componentLabels[] = { "Instance", "Reference", "X (mm)", "Y (mm)", "Rotation" };
    for( int c = 0; c < 5; ++c ) m_components->SetColLabelValue( c, componentLabels[c] );
    top->Add( m_components, 0, wxEXPAND | wxLEFT | wxRIGHT, 8 );
    top->Add( new wxStaticText( this, wxID_ANY, _( "Electrical links and endpoints" ) ), 0, wxLEFT | wxRIGHT, 8 );
    m_electrical = new wxGrid( this, wxID_ANY );
    m_electrical->CreateGrid( 0, 5 );
    const char* electricalLabels[] = { "Link ID", "System name", "Instance", "Reference", "Pad" };
    for( int c = 0; c < 5; ++c ) m_electrical->SetColLabelValue( c, electricalLabels[c] );
    m_electrical->SetMinSize( wxSize( -1, 100 ) );
    top->Add( m_electrical, 0, wxEXPAND | wxLEFT | wxRIGHT, 8 );
    m_status = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                               wxTE_READONLY ); top->Add( m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, 8 );
    auto* buttons = new wxWrapSizer( wxHORIZONTAL );
    buttons->Add( new wxButton( this, wxID_ADD, _( "Add Instance" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_EDIT, _( "Move Component…" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_DELETE, _( "Remove Instance" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_SAVE, _( "Save" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_OPEN, _( "Open" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_FORWARD, _( "Export STEP" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_REFRESH, _( "Check" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_HIGHEST + 101, _( "Add Mate" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_HIGHEST + 102, _( "Remove Mate" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_HIGHEST + 103, _( "View 3D" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_HIGHEST + 104, _( "Sync Electrical" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_HIGHEST + 105, _( "Add Endpoint" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_HIGHEST + 106, _( "Remove Endpoint" ) ), 0, wxALL, 2 );
    buttons->Add( new wxButton( this, wxID_CANCEL ), 0, wxALL, 2 ); top->Add( buttons, 0, wxEXPAND | wxALL, 8 );
    SetSizer( top );
    SetMinSize( wxSize( 1000, 780 ) );
    m_tabs->SetMinSize( wxSize( -1, 36 ) );
    m_instances->SetMinSize( wxSize( -1, 150 ) );
    m_mates->SetMinSize( wxSize( -1, 135 ) );
    m_components->SetMinSize( wxSize( -1, 155 ) );
    m_instances->SetColSize( 2, 340 );
    m_components->SetColSize( 0, 210 );
    m_electrical->SetColSize( 0, 190 );
    m_electrical->SetColSize( 1, 190 );
    m_electrical->SetColSize( 2, 210 );
    for( wxGrid* grid : { m_instances, m_mates, m_components, m_electrical } )
        grid->SetDefaultCellOverflow( false );
    for( int col : { 0, 2, 3, 4, 5, 7, 8 } ) m_mates->SetColSize( col, 135 );
    m_tabStates.push_back( { m_manifest, wxEmptyString, false } );
    RefreshInstances();
}

DIALOG_ASSEMBLY::~DIALOG_ASSEMBLY()
{
}

DIALOG_ASSEMBLY* DIALOG_ASSEMBLY::FindOrCreate( PCB_EDIT_FRAME* aParent )
{
    if( !aParent ) return nullptr;
    for( wxWindow* child : aParent->GetChildren() )
        if( auto* dialog = dynamic_cast<DIALOG_ASSEMBLY*>( child ) )
            return dialog;
    auto* dialog = new DIALOG_ASSEMBLY( aParent, ASSEMBLY_MANIFEST{} );
    dialog->Show();
    return dialog;
}
void DIALOG_ASSEMBLY::RefreshInstances()
{
    auto number = []( double value )
    {
        char buffer[64];
        auto converted = std::to_chars( buffer, buffer + sizeof( buffer ), value );
        return std::string( buffer, converted.ptr );
    };
    m_name->ChangeValue( wxString::FromUTF8( m_manifest.name ) );
    m_anchor->ChangeValue( wxString::FromUTF8( m_manifest.anchor ) );
    PCB_EDIT_FRAME* activeFrame = dynamic_cast<PCB_EDIT_FRAME*>( GetParent() );
    BOARD* activeBoard = activeFrame ? activeFrame->GetBoard() : nullptr;
    while( m_instances->GetNumberRows() ) m_instances->DeleteRows( 0 );
    if( !m_manifest.instances.empty() ) m_instances->AppendRows( m_manifest.instances.size() );
    for( size_t r = 0; r < m_manifest.instances.size(); ++r ) { const auto& i = m_manifest.instances[r];
        const std::string v[] = { i.id, i.kind, i.source, number( i.transform.translation[0] ), number( i.transform.translation[1] ), number( i.transform.translation[2] ), number( i.transform.rotation[0] ), number( i.transform.rotation[1] ), number( i.transform.rotation[2] ) };
        for( int c = 0; c < 9; ++c ) m_instances->SetCellValue( r, c, wxString::FromUTF8( v[c] ) );
        m_instances->SetCellValue( r, 9, i.folded ? "1" : "" ); }
    while( m_mates->GetNumberRows() ) m_mates->DeleteRows( 0 );
    if( !m_manifest.mates.empty() ) m_mates->AppendRows( m_manifest.mates.size() );
    for( size_t r = 0; r < m_manifest.mates.size(); ++r )
    {
        const auto& m = m_manifest.mates[r];
        const std::string first = number( m.first[0] ) + "," + number( m.first[1] ) + "," + number( m.first[2] );
        const std::string second = number( m.second[0] ) + "," + number( m.second[1] ) + "," + number( m.second[2] );
        const std::string axisA = number( m.first_axis[0] ) + "," + number( m.first_axis[1] ) + "," + number( m.first_axis[2] );
        const std::string axisB = number( m.second_axis[0] ) + "," + number( m.second_axis[1] ) + "," + number( m.second_axis[2] );
        const std::string values[] = { m.id, m.type, m.first_instance, m.second_instance, first, second,
                                       number( m.clearance_mm ), axisA, axisB, m.opposing ? "1" : "", m.type == "axis" ? "axis" : "point" };
        for( int c = 0; c < 11; ++c ) m_mates->SetCellValue( r, c, wxString::FromUTF8( values[c] ) );
    }
    while( m_components->GetNumberRows() ) m_components->DeleteRows( 0 );
    ASSEMBLY_MANIFEST componentManifest;
    try { componentManifest = FlattenAssembly( m_manifest ); }
    catch( const std::exception& e ) { m_status->SetValue( wxString::FromUTF8( e.what() ) ); }
    for( const auto& instance : componentManifest.instances )
    {
        if( instance.kind != "board" ) continue;
        try
        {
            const nlohmann::json components = AssemblyListComponents( componentManifest, instance.id, activeBoard );
            for( const auto& component : components.value( "components", nlohmann::json::array() ) )
            {
                const int row = m_components->GetNumberRows();
                m_components->AppendRows( 1 );
                m_components->SetCellValue( row, 0, wxString::FromUTF8( instance.id ) );
                m_components->SetCellValue( row, 1, wxString::FromUTF8( component.value( "reference", "" ) ) );
                m_components->SetCellValue( row, 2, wxString::Format( "%.6f", component.value( "x_mm", 0.0 ) ) );
                m_components->SetCellValue( row, 3, wxString::Format( "%.6f", component.value( "y_mm", 0.0 ) ) );
                m_components->SetCellValue( row, 4, wxString::Format( "%.6f", component.value( "rotation_deg", 0.0 ) ) );
            }
        }
        catch( const std::exception& e ) { m_status->SetValue( wxString::FromUTF8( e.what() ) ); }
    }
    while( m_electrical->GetNumberRows() ) m_electrical->DeleteRows( 0 );
    if( m_manifest.electrical.is_object() && m_manifest.electrical.contains( "links" )
        && m_manifest.electrical["links"].is_array() )
        for( const auto& link : m_manifest.electrical["links"] )
            if( link.is_object() && link.value( "endpoints", nlohmann::json::array() ).is_array() )
                for( const auto& endpoint : link["endpoints"] )
                    if( endpoint.is_object() )
                    {
                        const int row = m_electrical->GetNumberRows();
                        m_electrical->AppendRows( 1 );
                        m_electrical->SetCellValue( row, 0, wxString::FromUTF8( link.value( "id", "" ) ) );
                        m_electrical->SetCellValue( row, 1, wxString::FromUTF8( link.value( "system_name", link.value( "name", "" ) ) ) );
                        m_electrical->SetCellValue( row, 2, wxString::FromUTF8( endpoint.value( "instance", "" ) ) );
                        m_electrical->SetCellValue( row, 3, wxString::FromUTF8( endpoint.value( "reference", "" ) ) );
                        m_electrical->SetCellValue( row, 4, wxString::FromUTF8( endpoint.value( "pad", "" ) ) );
                    }
    const auto errors = m_manifest.Validate(); m_status->SetValue( errors.empty() ? _( "Manifest valid" ) : wxString::FromUTF8( errors.front() ) );
}
void DIALOG_ASSEMBLY::OnAdd( wxCommandEvent& )
{
    if( !SyncFromGrid() ) return;
    wxFileDialog dlg( this, _( "Choose PCB, STEP, or assembly source" ), wxEmptyString, wxEmptyString,
                      _( "PCB, STEP, and assembly files|*.kicad_pcb;*.step;*.stp;*.backplane-assembly.json|All files|*.*" ), wxFD_OPEN );
    if( dlg.ShowModal() != wxID_OK ) return;
    ASSEMBLY_INSTANCE i; i.id = "instance_" + std::to_string( m_manifest.instances.size() + 1 ); i.source = dlg.GetPath().ToStdString();
    const wxString ext = wxFileName( dlg.GetPath() ).GetExt().Lower();
    i.kind = ext == "json" ? "assembly" : ( ext == "step" || ext == "stp" ? "part" : "board" );
    m_manifest.instances.push_back( std::move( i ) ); m_dirty = true; RefreshInstances();
}
void DIALOG_ASSEMBLY::OnEdit( wxCommandEvent& )
{
    if( !SyncFromGrid() ) return;
    const int row = m_components->GetGridCursorRow();
    if( row < 0 || row >= m_components->GetNumberRows() )
    { m_status->SetValue( _( "Select a source component first" ) ); return; }
    const wxString reference = m_components->GetCellValue( row, 1 );
    wxTextEntryDialog editor( this, _( "Source PCB position: X mm, Y mm, rotation degrees" ),
            _( "Move component " ) + reference, m_components->GetCellValue( row, 2 ) + ", "
            + m_components->GetCellValue( row, 3 ) + ", " + m_components->GetCellValue( row, 4 ) );
    if( editor.ShowModal() != wxID_OK ) return;
    try
    {
        wxStringTokenizer tokens( editor.GetValue(), "," );
        if( tokens.CountTokens() != 3 ) throw std::invalid_argument( "Enter X, Y, and rotation" );
        double values[3];
        for( double& value : values )
            if( !tokens.GetNextToken().ToDouble( &value ) || !std::isfinite( value ) )
                throw std::invalid_argument( "Enter three finite numbers" );
        auto* frame = dynamic_cast<PCB_EDIT_FRAME*>( GetParent() );
        AssemblyMoveComponent( m_manifest, m_components->GetCellValue( row, 0 ).ToStdString(),
                reference.ToStdString(), values[0], values[1], values[2], frame,
                frame ? frame->GetBoard() : nullptr );
        RefreshInstances();
        if( !m_tabStates[m_tabs->GetSelection()].stagingStep.empty() )
        { wxCommandEvent refresh; OnView3D( refresh ); }
        m_status->SetValue( _( "Source component moved" ) );
    }
    catch( const std::exception& e ) { m_status->SetValue( wxString::FromUTF8( e.what() ) ); }
}

void DIALOG_ASSEMBLY::OnGridChanged( wxGridEvent& aEvent )
{
    m_dirty = true;
    aEvent.Skip();
}
void DIALOG_ASSEMBLY::OnRemove( wxCommandEvent& )
{
    const int r = m_instances->GetGridCursorRow();
    if( r >= 0 && r < (int) m_manifest.instances.size() ) { if( !SyncFromGrid() ) return; m_manifest.instances.erase( m_manifest.instances.begin() + r ); m_dirty = true; RefreshInstances(); }
}
void DIALOG_ASSEMBLY::OnSave( wxCommandEvent& )
{
    wxFileDialog dlg( this, _( "Save assembly manifest" ), wxEmptyString, "assembly.backplane-assembly.json",
                      _( "Assembly manifest (*.backplane-assembly.json)|*.backplane-assembly.json" ), wxFD_SAVE | wxFD_OVERWRITE_PROMPT );
    if( dlg.ShowModal() == wxID_OK ) { m_manifestPath = dlg.GetPath(); SaveManifest( m_manifestPath ); }
}
void DIALOG_ASSEMBLY::OnOpen( wxCommandEvent& )
{
    wxFileDialog dlg( this, _( "Open assembly manifest" ), wxEmptyString, wxEmptyString,
                      _( "Assembly manifests (*.backplane-assembly.json)|*.backplane-assembly.json" ), wxFD_OPEN | wxFD_FILE_MUST_EXIST );
    if( dlg.ShowModal() == wxID_OK )
    {
        try
        {
            OpenTab( dlg.GetPath() );
        }
        catch( const std::exception& e )
        {
            m_status->SetValue( wxString::FromUTF8( e.what() ) );
        }
    }
}
void DIALOG_ASSEMBLY::OnCheck( wxCommandEvent& )
{
    if( !SyncFromGrid() ) return;
    try
    {
    const auto collisions = ASSEMBLY_EXPORTER::CheckCollisions( m_manifest, &NULL_REPORTER::GetInstance(),
        [this]( const ASSEMBLY_INSTANCE& i, const std::string& out, REPORTER* reporter )
        {
            if( i.kind != "board" ) return true;
            const auto path = m_manifest.base_directory / i.source;
            std::unique_ptr<BOARD> board = PCB_IO_MGR::Load( PCB_IO_MGR::FindPluginTypeFromBoardPath( wxString::FromUTF8( path.string() ) ), wxString::FromUTF8( path.string() ) );
            if( !board ) return false; EXPORTER_STEP_PARAMS params; EXPORTER_STEP exporter( board.get(), params, reporter ); exporter.m_outputFile = wxString::FromUTF8( out );
            return exporter.Export();
        } );
    m_status->SetValue( wxString::Format( _( "%zu collision(s) found" ), collisions.size() ) );
    }
    catch( const std::exception& e )
    {
        m_status->SetValue( wxString::FromUTF8( e.what() ) );
    }
}
void DIALOG_ASSEMBLY::OnExport( wxCommandEvent& )
{
    if( !SyncFromGrid() ) return;
    wxFileDialog dlg( this, _( "Export assembly STEP" ), wxEmptyString, "assembly.step", _( "STEP files (*.step)|*.step" ), wxFD_SAVE | wxFD_OVERWRITE_PROMPT );
    if( dlg.ShowModal() != wxID_OK ) return;
    const bool ok = ASSEMBLY_EXPORTER::Export( m_manifest, dlg.GetPath().ToStdString(), &NULL_REPORTER::GetInstance(),
        [this]( const ASSEMBLY_INSTANCE& i, const std::string& out, REPORTER* reporter )
        {
            if( i.kind != "board" ) return true;
            const auto path = m_manifest.base_directory / i.source;
            std::unique_ptr<BOARD> board = PCB_IO_MGR::Load( PCB_IO_MGR::FindPluginTypeFromBoardPath( wxString::FromUTF8( path.string() ) ), wxString::FromUTF8( path.string() ) );
            if( !board ) return false;
            EXPORTER_STEP_PARAMS params; params.m_Format = EXPORTER_STEP_PARAMS::FORMAT::STEP; params.m_BoardOnly = false;
            EXPORTER_STEP exporter( board.get(), params, reporter ); exporter.m_outputFile = wxString::FromUTF8( out );
            return exporter.Export();
        } );
    m_status->SetValue( ok ? _( "Assembly STEP exported" ) : _( "Assembly STEP export failed" ) );
}
bool DIALOG_ASSEMBLY::SyncFromGrid()
{
    if( !m_instances || !m_mates || !m_electrical ) return true;
    for( wxGrid* grid : { m_instances, m_mates, m_components, m_electrical } )
        if( grid->IsCellEditControlEnabled() ) grid->SaveEditControlValue();
    ASSEMBLY_MANIFEST previous = m_manifest;
    try
    {
    m_manifest.name = m_name->GetValue().ToStdString();
    m_manifest.anchor = m_anchor->GetValue().ToStdString();
    auto number = []( const wxString& text, double* value )
    {
        if( !text.ToDouble( value ) || !std::isfinite( *value ) )
            throw std::invalid_argument( "Enter finite numeric positions, rotations, and clearances" );
    };

    for( size_t r = 0; r < m_manifest.instances.size() && r < (size_t) m_instances->GetNumberRows(); ++r ) { auto& i = m_manifest.instances[r];
        i.id = m_instances->GetCellValue(r,0).ToStdString(); i.kind = m_instances->GetCellValue(r,1).ToStdString(); i.source = m_instances->GetCellValue(r,2).ToStdString();
        double* vals[] = { &i.transform.translation[0], &i.transform.translation[1], &i.transform.translation[2], &i.transform.rotation[0], &i.transform.rotation[1], &i.transform.rotation[2] };
        for( int c=0;c<6;++c ) number( m_instances->GetCellValue(r,c+3), vals[c] );
        i.folded = m_instances->GetCellValue( r, 9 ) == "1" || m_instances->GetCellValue( r, 9 ).CmpNoCase( "true" ) == 0;
    }
    for( size_t r = 0; r < m_manifest.mates.size() && r < (size_t) m_mates->GetNumberRows(); ++r )
    {
        auto& m = m_manifest.mates[r];
        m.id = m_mates->GetCellValue( r, 0 ).ToStdString();
        m.type = m_mates->GetCellValue( r, 1 ).ToStdString();
        m.first_instance = m_mates->GetCellValue( r, 2 ).ToStdString();
        m.second_instance = m_mates->GetCellValue( r, 3 ).ToStdString();
        auto parsePoint = [&]( int col, std::array<double, 3>& point )
        {
            wxString value = m_mates->GetCellValue( r, col );
            wxStringTokenizer tokens( value, "," );
            if( tokens.CountTokens() != 3 ) throw std::invalid_argument( "Points and axes require three comma-separated numbers" );
            for( size_t k = 0; k < 3; ++k ) number( tokens.GetNextToken(), &point[k] );
        };
        parsePoint( 4, m.first ); parsePoint( 5, m.second );
        number( m_mates->GetCellValue( r, 6 ), &m.clearance_mm );
        parsePoint( 7, m.first_axis ); parsePoint( 8, m.second_axis );
        m.opposing = m_mates->GetCellValue( r, 9 ) == "1" || m_mates->GetCellValue( r, 9 ).CmpNoCase( "true" ) == 0;
    }
    SyncElectricalGrid();
    return true;
    }
    catch( const std::exception& e )
    {
        m_manifest = std::move( previous );
        m_status->SetValue( wxString::FromUTF8( e.what() ) );
        return false;
    }
}

void DIALOG_ASSEMBLY::SyncElectricalGrid()
{
    if( !m_electrical ) return;
    nlohmann::json existing = nlohmann::json::object();
    if( m_manifest.electrical.is_object() && m_manifest.electrical.contains( "links" )
        && m_manifest.electrical["links"].is_array() )
        for( const auto& link : m_manifest.electrical["links"] )
            if( link.is_object() ) existing[link.value( "id", "" )] = link;
    nlohmann::json links = nlohmann::json::array();
    std::map<std::string, size_t> indexes;
    for( int row = 0; row < m_electrical->GetNumberRows(); ++row )
    {
        const std::string id = m_electrical->GetCellValue( row, 0 ).ToStdString();
        if( id.empty() ) continue;
        size_t index;
        if( !indexes.contains( id ) )
        {
            nlohmann::json link = existing.contains( id ) ? existing[id] : nlohmann::json::object();
            link["id"] = id;
            link["name"] = m_electrical->GetCellValue( row, 1 ).ToStdString();
            link.erase( "system_name" );
            link["endpoints"] = nlohmann::json::array();
            index = links.size(); indexes[id] = index; links.push_back( std::move( link ) );
        }
        else index = indexes[id];
        links[index]["endpoints"].push_back( { { "instance", m_electrical->GetCellValue( row, 2 ).ToStdString() },
                                                 { "reference", m_electrical->GetCellValue( row, 3 ).ToStdString() },
                                                 { "pad", m_electrical->GetCellValue( row, 4 ).ToStdString() } } );
    }
    if( !m_manifest.electrical.is_object() ) m_manifest.electrical = nlohmann::json::object();
    m_manifest.electrical["links"] = std::move( links );
}
bool DIALOG_ASSEMBLY::SaveManifest( const wxString& path )
{
    try
    {
        if( !SyncFromGrid() ) return false;
        m_manifest.Save( path.ToStdString() );
        m_manifestPath = path;
        m_dirty = false;
        if( !StoreCurrentTab() ) return false;
        RefreshInstances();
        return true;
    }
    catch( const std::exception& e ) { m_status->SetValue( wxString::FromUTF8( e.what() ) ); return false; }
}

bool DIALOG_ASSEMBLY::LoadManifest( const wxString& path )
{
    try
    {
        if( !StoreCurrentTab() ) return false;
        m_manifest = ASSEMBLY_MANIFEST::Load( path.ToStdString() );
        m_manifestPath = path;
        m_dirty = false;
        RefreshInstances();
        return true;
    }
    catch( const std::exception& e ) { m_status->SetValue( wxString::FromUTF8( e.what() ) ); return false; }
}

bool DIALOG_ASSEMBLY::OpenTab( const wxString& path )
{
    ASSEMBLY_MANIFEST opened = ASSEMBLY_MANIFEST::Load( path.ToStdString() );
    const auto errors = opened.Validate( true );
    if( !errors.empty() ) throw std::invalid_argument( errors.front() );
    if( !StoreCurrentTab() ) return false;
    m_tabStates.push_back( { std::move( opened ), path, false } );
    // ChangeSelection does not emit notebook events; the old tab is already saved.
    m_tabs->AddPage( new wxPanel( m_tabs ), wxFileName( path ).GetName(), false );
    m_tabs->ChangeSelection( m_tabStates.size() - 1 );
    LoadTab( m_tabStates.size() - 1 );
    Show();
    Raise();
    return true;
}

nlohmann::json DIALOG_ASSEMBLY::GetOpenTabs()
{
    if( !StoreCurrentTab() ) throw std::invalid_argument( "Current assembly has invalid edits" );
    nlohmann::json result = nlohmann::json::array();
    for( size_t i = 0; i < m_tabStates.size(); ++i )
        result.push_back( { { "index", i }, { "path", m_tabStates[i].path.ToStdString() },
                            { "name", m_tabStates[i].manifest.name }, { "dirty", m_tabStates[i].dirty },
                            { "viewer_open", false },
                            { "unfolded_step", m_tabStates[i].stagingStep.string() } } );
    return result;
}

bool DIALOG_ASSEMBLY::ShowTab3D( size_t aTab )
{
    if( aTab >= m_tabStates.size() ) return false;
    if( m_tabs->GetSelection() != static_cast<int>( aTab ) )
    {
        if( !StoreCurrentTab() ) return false;
        m_tabs->ChangeSelection( static_cast<int>( aTab ) );
        LoadTab( aTab );
    }
    wxCommandEvent event;
    OnView3D( event );
    return !m_tabStates[aTab].stagingStep.empty();
}

bool DIALOG_ASSEMBLY::StoreCurrentTab()
{
    if( !m_tabs || m_tabStates.empty() ) return true;
    return StoreTab( m_tabs->GetSelection() );
}

bool DIALOG_ASSEMBLY::StoreTab( size_t index )
{
    if( !SyncFromGrid() ) return false;
    if( index < m_tabStates.size() )
    {
        m_tabStates[index].manifest = m_manifest;
        m_tabStates[index].path = m_manifestPath;
        m_tabStates[index].dirty = m_dirty;
        m_tabs->SetPageText( index, ( m_manifestPath.empty() ? _( "Untitled assembly" )
                : wxFileName( m_manifestPath ).GetName() ) + ( m_dirty ? " *" : "" ) );
    }
    return true;
}

void DIALOG_ASSEMBLY::LoadTab( size_t aIndex )
{
    if( aIndex >= m_tabStates.size() ) return;
    m_manifest = m_tabStates[aIndex].manifest;
    m_manifestPath = m_tabStates[aIndex].path;
    m_dirty = m_tabStates[aIndex].dirty;
    RefreshInstances();
}

void DIALOG_ASSEMBLY::OnTabChanged( wxBookCtrlEvent& event )
{
    if( event.GetOldSelection() >= 0 && !StoreTab( event.GetOldSelection() ) )
    {
        m_tabs->ChangeSelection( event.GetOldSelection() );
        return;
    }
    LoadTab( event.GetSelection() );
    event.Skip();
}

void DIALOG_ASSEMBLY::OnCancel( wxCommandEvent& )
{
    Close();
}

void DIALOG_ASSEMBLY::OnCloseWindow( wxCloseEvent& aEvent )
{
    if( !StoreCurrentTab() ) { aEvent.Veto(); return; }
    const bool anyDirty = std::any_of( m_tabStates.begin(), m_tabStates.end(),
                                       []( const TAB_STATE& aTab ) { return aTab.dirty; } );
    if( anyDirty )
    {
        const int answer = wxMessageBox( _( "This assembly has unsaved changes. Save before closing?" ),
                                         _( "Assembly" ), wxYES_NO | wxCANCEL | wxICON_QUESTION, this );
        if( answer == wxCANCEL ) { aEvent.Veto(); return; }
        if( answer == wxYES )
        {
            bool failed = false;
            for( size_t i = 0; i < m_tabStates.size(); ++i )
            {
                if( !m_tabStates[i].dirty ) continue;
                if( m_tabStates[i].path.IsEmpty() )
                {
                    m_tabs->ChangeSelection( i );
                    LoadTab( i );
                    wxCommandEvent save;
                    OnSave( save );
                    StoreCurrentTab();
                }
                else
                {
                    try
                    {
                        m_tabStates[i].manifest.Save( m_tabStates[i].path.ToStdString() );
                        m_tabStates[i].dirty = false;
                    }
                    catch( const std::exception& e ) { m_status->SetValue( wxString::FromUTF8( e.what() ) ); failed = true; }
                }
            }
            if( failed || std::any_of( m_tabStates.begin(), m_tabStates.end(),
                                       []( const TAB_STATE& aTab ) { return aTab.dirty; } ) )
            { aEvent.Veto(); return; }
        }
    }
    if( m_closedCallback ) m_closedCallback();
    Destroy();
}

void DIALOG_ASSEMBLY::OnAddMate( wxCommandEvent& )
{
    if( !SyncFromGrid() ) return;
    if( m_manifest.instances.size() < 2 ) { m_status->SetValue( _( "Add two instances before creating a mate" ) ); return; }
    ASSEMBLY_MATE mate;
    mate.id = "mate_" + std::to_string( m_manifest.mates.size() + 1 );
    mate.first_instance = m_manifest.instances[0].id;
    mate.second_instance = m_manifest.instances[1].id;
    m_manifest.mates.push_back( mate );
    m_dirty = true;
    RefreshInstances();
}

void DIALOG_ASSEMBLY::OnRemoveMate( wxCommandEvent& )
{
    const int row = m_mates->GetGridCursorRow();
    if( row >= 0 && row < static_cast<int>( m_manifest.mates.size() ) )
    {
        if( !SyncFromGrid() ) return;
        m_manifest.mates.erase( m_manifest.mates.begin() + row );
        m_dirty = true;
        RefreshInstances();
    }
}

void DIALOG_ASSEMBLY::OnElectricalSync( wxCommandEvent& )
{
    try
    {
        if( !SyncFromGrid() ) return;
        PCB_EDIT_FRAME* frame = dynamic_cast<PCB_EDIT_FRAME*>( GetParent() );
        const nlohmann::json report = AssemblyElectricalSync( m_manifest, frame ? frame->GetBoard() : nullptr );
        if( report.at( "errors" ).empty() )
        {
            m_manifest.electrical["snapshot"] = report;
            m_dirty = true;
        }
        m_status->SetValue( wxString::FromUTF8( report.dump() ) );
    }
    catch( const std::exception& e ) { m_status->SetValue( wxString::FromUTF8( e.what() ) ); }
}

void DIALOG_ASSEMBLY::OnAddEndpoint( wxCommandEvent& )
{
    const int row = m_electrical->GetNumberRows();
    m_electrical->AppendRows( 1 );
    m_electrical->SetCellValue( row, 0, "link_1" );
    m_electrical->SetCellValue( row, 1, "system" );
    m_dirty = true;
}

void DIALOG_ASSEMBLY::OnRemoveEndpoint( wxCommandEvent& )
{
    const int row = m_electrical->GetGridCursorRow();
    if( row >= 0 && row < m_electrical->GetNumberRows() )
    {
        m_electrical->DeleteRows( row );
        m_dirty = true;
    }
}

void DIALOG_ASSEMBLY::OnView3D( wxCommandEvent& )
{
    if( !SyncFromGrid() ) return;
    const auto errors = m_manifest.Validate( true );
    if( !errors.empty() ) { m_status->SetValue( wxString::FromUTF8( errors.front() ) ); return; }

    PCB_EDIT_FRAME* frame = dynamic_cast<PCB_EDIT_FRAME*>( GetParent() );
    if( !frame ) { m_status->SetValue( _( "Assembly 3D requires the PCB editor" ) ); return; }

    try
    {
        TAB_STATE& tab = m_tabStates[m_tabs->GetSelection()];
        if( tab.stagingDirectory.empty() )
        {
            tab.stagingDirectory = std::filesystem::temp_directory_path() /
                                  ( "backplane-assembly-view-" + std::to_string( reinterpret_cast<uintptr_t>( this ) ) + "-" + std::to_string( m_tabs->GetSelection() ) );
            std::filesystem::create_directories( tab.stagingDirectory );
        }
        const auto step = tab.stagingDirectory / ( "assembly-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count() ) + ".step" );
        const bool exported = ASSEMBLY_EXPORTER::Export( m_manifest, step.string(), &NULL_REPORTER::GetInstance(),
            [this]( const ASSEMBLY_INSTANCE& i, const std::string& out, REPORTER* reporter )
            {
                if( i.kind != "board" ) return true;
                auto path = m_manifest.base_directory / i.source;
                if( !std::filesystem::exists( path ) ) path = i.source;
                auto* editor = dynamic_cast<PCB_EDIT_FRAME*>( GetParent() );
                BOARD* source = editor ? editor->GetBoard() : nullptr;
                std::error_code ec;
                std::unique_ptr<BOARD> loaded;
                if( !source || !std::filesystem::equivalent( path, source->GetFileName().ToStdString(), ec ) || ec )
                {
                    loaded = PCB_IO_MGR::Load( PCB_IO_MGR::KICAD_SEXP,
                                               wxString::FromUTF8( path.string() ) );
                    source = loaded.get();
                }
                if( !source ) return false;
                EXPORTER_STEP_PARAMS params;
                // TODO(assembly): pass the instance's folded state once the
                // rigid-flex STEP/exporter hook lands. This path is unfolded.
                EXPORTER_STEP exporter( source, params, reporter );
                exporter.m_outputFile = wxString::FromUTF8( out );
                return exporter.Export();
            } );
        if( !exported ) throw std::runtime_error( "assembly STEP export failed" );

        STEPControl_Reader reader;
        if( reader.ReadFile( step.string().c_str() ) != IFSelect_RetDone || reader.TransferRoots() == 0 )
            throw std::runtime_error( "Could not read assembly model bounds" );
        Bnd_Box bounds;
        BRepBndLib::Add( reader.OneShape(), bounds );
        if( bounds.IsVoid() ) throw std::runtime_error( "Assembly has no displayable geometry" );
        tab.stagingStep = step;
        // TODO(assembly): hand this unfolded STEP to the assembly-capable 3D
        // viewer when the separate 3D-viewer implementation lands. The
        // exporter and bounds path are intentionally functional on master;
        // folded rigid-flex presentation is not part of this port.
        m_status->SetValue( _( "Unfolded assembly STEP prepared for 3D viewing" ) );
    }
    catch( const std::exception& e ) { m_status->SetValue( wxString::FromUTF8( e.what() ) ); }
}
