#include "board_structure_tool.h"

#include <board_structure.h>
#include <dialogs/dialog_structure_stack_preset.h>
#include <dialogs/dialog_structure_properties.h>
#include <wx/msgdlg.h>
#include <pcb_edit_frame.h>
#include <tool/tool_event.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_grid_helper.h>
#include <geometry/shape_poly_set.h>
#include <view/view_controls.h>
#include <wx/choicdlg.h>
#include <wx/intl.h>

namespace
{
VECTOR2I eventPosition( TOOL_EVENT* aEvent, KIGFX::VIEW_CONTROLS* aControls )
{
    return aEvent->HasPosition() ? aEvent->Position() : aControls->GetMousePosition();
}

template <typename T>
std::string nextId( const std::vector<T>& aItems, const char* aPrefix )
{
    unsigned n = 1;
    for( ;; ++n )
    {
        std::string candidate = std::string( aPrefix ) + std::to_string( n );
        bool used = false;
        for( const auto& item : aItems )
            if( item.id == candidate ) { used = true; break; }
        if( !used ) return candidate;
    }
}

const BOARD_STRUCTURE_REGION* chooseRegion( PCB_EDIT_FRAME* aFrame, const VECTOR2I& aPoint )
{
    BOARD_STRUCTURE& structure = aFrame->GetBoard()->GetBoardStructure();
    if( const BOARD_STRUCTURE_REGION* region = structure.RegionAt( aPoint ) )
        return region;

    if( structure.regions.empty() )
        return nullptr;
    wxArrayString choices;
    for( const auto& region : structure.regions )
        choices.Add( wxString::FromUTF8( region.name.empty() ? region.id : region.name ) );
    wxSingleChoiceDialog dialog( aFrame, _( "Choose the region for this feature:" ),
                                 _( "Board Structure Region" ), choices );
    if( dialog.ShowModal() != wxID_OK ) return nullptr;
    return &structure.regions[dialog.GetSelection()];
}


}

BOARD_STRUCTURE_TOOL::BOARD_STRUCTURE_TOOL() : PCB_TOOL_BASE( "pcbnew.BoardStructureTool" ) {}

BOARD_STRUCTURE_TOOL::~BOARD_STRUCTURE_TOOL()
{
    if( getView() )
        getView()->Remove( &m_preview );
}

bool BOARD_STRUCTURE_TOOL::Init()
{
    view()->Add( &m_preview );
    view()->SetVisible( &m_preview, false );
    return PCB_TOOL_BASE::Init();
}

void BOARD_STRUCTURE_TOOL::setTransitions()
{
    Go( &BOARD_STRUCTURE_TOOL::EditFeature, PCB_ACTIONS::editBoardStructureFeature.MakeEvent() );
    Go( &BOARD_STRUCTURE_TOOL::DrawBoundary, PCB_ACTIONS::drawStructureBoundary.MakeEvent() );
    Go( &BOARD_STRUCTURE_TOOL::AssignStack, PCB_ACTIONS::assignStructureStack.MakeEvent() );
    Go( &BOARD_STRUCTURE_TOOL::EditBoundary, PCB_ACTIONS::editStructureBoundary.MakeEvent() );
    Go( &BOARD_STRUCTURE_TOOL::DrawRegion, PCB_ACTIONS::drawBoardStructureRegion.MakeEvent() );
    Go( &BOARD_STRUCTURE_TOOL::DrawRegion, PCB_ACTIONS::drawBoardStructureRigidRegion.MakeEvent() );
    Go( &BOARD_STRUCTURE_TOOL::DrawRegion, PCB_ACTIONS::drawBoardStructureFlexRegion.MakeEvent() );
    Go( &BOARD_STRUCTURE_TOOL::DrawBend, PCB_ACTIONS::drawBoardStructureBend.MakeEvent() );
    Go( &BOARD_STRUCTURE_TOOL::DrawStiffener, PCB_ACTIONS::drawBoardStructureStiffener.MakeEvent() );
}

int BOARD_STRUCTURE_TOOL::DrawBoundary( const TOOL_EVENT& aEvent )
{
    return drawBoundary( aEvent );
}

int BOARD_STRUCTURE_TOOL::drawBoundary( const TOOL_EVENT& aEvent )
{
    auto* editFrame = dynamic_cast<PCB_EDIT_FRAME*>( frame() );
    if( !editFrame ) return 0;
    Activate();
    PCB_GRID_HELPER grid( m_toolMgr, editFrame->GetMagneticItemsSettings() );
    LSET layers;
    layers.set();
    std::vector<VECTOR2I> points;
    bool done = false;
    m_preview.SetPoints( {}, {}, {} );
    view()->Add( &m_preview );
    view()->SetVisible( &m_preview, true );
    m_preview.SetLineColor( KIGFX::COLOR4D( 1.0, 0.55, 0.1, 1.0 ) );
    m_preview.SetFillColor( KIGFX::COLOR4D( 1.0, 0.55, 0.1, 0.0 ) );
    auto clearPreview = [&]()
    { view()->SetVisible( &m_preview, false ); view()->Update( &m_preview, KIGFX::GEOMETRY ); };

    while( TOOL_EVENT* event = Wait() )
    {
        if( event->IsCancelInteractive() || event->IsActivate() ) break;
        if( event->IsClick( BUT_RIGHT ) ) { done = points.size() >= 2; break; }
        if( event->IsClick( BUT_LEFT ) || event->IsDblClick( BUT_LEFT ) )
        {
            const VECTOR2I point = grid.ResolveSnap( eventPosition( event, controls() ), layers,
                                                        GRID_HELPER_GRIDS::GRID_GRAPHICS ).position;
            if( event->IsDblClick( BUT_LEFT ) )
            {
                if( points.empty() || point != points.back() ) points.push_back( point );
                done = points.size() >= 2;
                break;
            }
            points.push_back( point );
        }
        else if( event->IsMotion() && !points.empty() )
        {
            const VECTOR2I point = grid.ResolveSnap( eventPosition( event, controls() ), layers,
                                                        GRID_HELPER_GRIDS::GRID_GRAPHICS ).position;
            SHAPE_LINE_CHAIN locked, leader;
            for( const VECTOR2I& p : points ) locked.Append( p );
            leader.Append( points.back() ); leader.Append( point );
            m_preview.SetPoints( locked, leader, {} );
            view()->Update( &m_preview, KIGFX::GEOMETRY );
        }
        else event->SetPassEvent();
    }
    clearPreview();
    if( !done ) return 0;

    BOARD_STRUCTURE next = editFrame->GetBoard()->GetBoardStructure();
    BOARD_STRUCTURE_BOUNDARY boundary;
    boundary.id = nextId( next.boundaries, "boundary-" );
    boundary.polyline = std::move( points );
    next.boundaries.push_back( std::move( boundary ) );
    SHAPE_POLY_SET outline;
    if( !editFrame->GetBoard()->GetBoardPolygonOutlines( outline, false ) )
    {
        wxMessageBox( _( "The board has no valid Edge.Cuts outline." ), _( "Invalid Structure Boundary" ),
                      wxOK | wxICON_ERROR, editFrame );
        return 0;
    }
    std::string error;
    if( !next.DerivePartition( outline, &error ) )
    {
        wxMessageBox( wxString::FromUTF8( error ), _( "Invalid Structure Boundary" ),
                      wxOK | wxICON_ERROR, editFrame );
        return 0;
    }
    if( std::any_of( next.regions.begin(), next.regions.end(),
                     [&]( const auto& r ) { return !next.StackForRegion( r ); } ) )
    {
        std::string stackId;
        if( !ChooseStructureStack( editFrame, next, BOARD_STRUCTURE_STACK_KIND::RIGID, stackId ) )
            return 0;
        for( auto& region : next.regions )
            if( !next.StackForRegion( region ) ) region.stack_id = stackId;
    }
    for( auto& region : next.regions ) if( region.name.empty() ) region.name = region.id;
    const auto validation = next.Validate();
    if( !validation.IsValid() )
    {
        wxMessageBox( wxString::FromUTF8( validation.errors.front() ),
                      _( "Invalid Structure Boundary" ), wxOK | wxICON_ERROR, editFrame );
        return 0;
    }
    editFrame->CommitBoardStructure( next, _( "Draw structure boundary" ) );
    return 0;
}

int BOARD_STRUCTURE_TOOL::AssignStack( const TOOL_EVENT& aEvent )
{
    auto* editFrame = dynamic_cast<PCB_EDIT_FRAME*>( frame() );
    if( !editFrame ) return 0;
    Activate();
    while( TOOL_EVENT* event = Wait() )
    {
        if( event->IsCancelInteractive() || event->IsActivate() ) break;
        if( !event->IsClick( BUT_LEFT ) ) { event->SetPassEvent(); continue; }
        const VECTOR2I point = eventPosition( event, controls() );
        BOARD_STRUCTURE next = editFrame->GetBoard()->GetBoardStructure();
        const auto* region = next.RegionAt( point );
        if( !region ) continue;
        const auto index = static_cast<size_t>( region - next.regions.data() );
        wxArrayString kinds{ _( "Rigid" ), _( "Flex" ) };
        wxSingleChoiceDialog chooser( editFrame, _( "Choose the stack kind for this region:" ),
                                      _( "Assign Structure Stack" ), kinds );
        if( chooser.ShowModal() != wxID_OK ) break;
        const auto kind = chooser.GetSelection() == 1 ? BOARD_STRUCTURE_STACK_KIND::FLEX
                                                       : BOARD_STRUCTURE_STACK_KIND::RIGID;
        std::string stackId;
        if( !ChooseStructureStack( editFrame, next, kind, stackId ) ) break;
        next.regions[index].stack_id = stackId;
        // Also repair unassigned draft partitions from older builds.
        for( auto& r : next.regions )
        {
            if( !next.StackForRegion( r ) ) r.stack_id = stackId;
            if( r.name.empty() ) r.name = r.id;
        }
        if( EditStructureFeatureProperties( editFrame, next, 0, index ) )
            editFrame->CommitBoardStructure( next, _( "Assign structure stack" ) );
        break;
    }
    return 0;
}

void BOARD_STRUCTURE_TOOL::Reset( RESET_REASON aReason )
{
    PCB_TOOL_BASE::Reset( aReason );
    view()->Add( &m_preview );
    view()->SetVisible( &m_preview, false );
}

int BOARD_STRUCTURE_TOOL::DrawRegion( const TOOL_EVENT& aEvent )
{
    int kind = -1;
    if( aEvent.IsAction( &PCB_ACTIONS::drawBoardStructureRigidRegion ) )
        kind = static_cast<int>( BOARD_STRUCTURE_STACK_KIND::RIGID );
    else if( aEvent.IsAction( &PCB_ACTIONS::drawBoardStructureFlexRegion ) )
        kind = static_cast<int>( BOARD_STRUCTURE_STACK_KIND::FLEX );
    return drawPolygon( aEvent, false, kind );
}
int BOARD_STRUCTURE_TOOL::DrawStiffener( const TOOL_EVENT& aEvent ) { return drawPolygon( aEvent, true ); }

int BOARD_STRUCTURE_TOOL::drawPolygon( const TOOL_EVENT& aEvent, bool aStiffener, int aStackKind )
{
    PCB_EDIT_FRAME* editFrame = dynamic_cast<PCB_EDIT_FRAME*>( frame() );
    if( !editFrame ) return 0;
    Activate();
    controls()->ShowCursor( true );
    controls()->ForceCursorPosition( false );
    PCB_GRID_HELPER grid( m_toolMgr, editFrame->GetMagneticItemsSettings() );
    LSET layers;
    layers.set();
    std::vector<VECTOR2I> points;
    bool done = false;
    m_preview.SetPoints( {}, {}, {} );
    view()->Add( &m_preview );
    view()->SetVisible( &m_preview, true );
    m_preview.SetLineColor( KIGFX::COLOR4D( 0.2, 0.85, 1.0, 1.0 ) );
    m_preview.SetFillColor( KIGFX::COLOR4D( 0.2, 0.85, 1.0, 0.12 ) );
    auto clearPreview = [&]()
    {
        view()->SetVisible( &m_preview, false );
        view()->Update( &m_preview, KIGFX::GEOMETRY );
    };
    while( TOOL_EVENT* event = Wait() )
    {
        if( event->IsCancelInteractive() || event->IsActivate() ) break;
        if( event->IsClick( BUT_RIGHT ) ) { done = points.size() >= 3; break; }
        if( event->IsClick( BUT_LEFT ) || event->IsDblClick( BUT_LEFT ) )
        {
            VECTOR2I point = grid.ResolveSnap( eventPosition( event, controls() ), layers,
                                                  GRID_HELPER_GRIDS::GRID_GRAPHICS ).position;
            if( points.size() >= 3 && point == points.front() ) { done = true; break; }
            if( event->IsDblClick( BUT_LEFT ) )
            {
                if( points.empty() || point != points.back() ) points.push_back( point );
                done = points.size() >= 3;
                break;
            }
            points.push_back( point );
            if( event->IsDblClick( BUT_LEFT ) ) { done = points.size() >= 3; break; }
            SHAPE_LINE_CHAIN locked;
            for( const VECTOR2I& p : points ) locked.Append( p );
            SHAPE_LINE_CHAIN leader;
            SHAPE_LINE_CHAIN loop;
            m_preview.SetPoints( locked, leader, loop );
            view()->Update( &m_preview, KIGFX::GEOMETRY );
        }
        else if( event->IsMotion() && !points.empty() )
        {
            VECTOR2I point = grid.ResolveSnap( eventPosition( event, controls() ), layers,
                                                  GRID_HELPER_GRIDS::GRID_GRAPHICS ).position;
            SHAPE_LINE_CHAIN locked, leader, loop;
            for( const VECTOR2I& p : points ) locked.Append( p );
            leader.Append( points.back() );
            leader.Append( point );
            loop.Append( point );
            loop.Append( points.front() );
            m_preview.SetPoints( locked, leader, loop );
            view()->Update( &m_preview, KIGFX::GEOMETRY );
        }
        else
            event->SetPassEvent();
    }
    clearPreview();
    controls()->ForceCursorPosition( false );
    if( !done ) return 0;

    BOARD_STRUCTURE next = editFrame->GetBoard()->GetBoardStructure();
    if( aStiffener )
    {
        const BOARD_STRUCTURE_REGION* region = chooseRegion( editFrame, points.front() );
        if( !region ) return 0;
        BOARD_STRUCTURE_STIFFENER stiffener;
        stiffener.id = nextId( next.stiffeners, "stiffener-" );
        stiffener.region_id = region->id;
        stiffener.material = "FR4";
        stiffener.adhesive = "Acrylic";
        stiffener.thickness_nm = 100000;
        if( editFrame->GetCanvas()->GetActiveConstructionLayer() == CONSTRUCTION_LAYER::B_STIFFENER )
            stiffener.top = false;
        stiffener.shape.outline = std::move( points );
        next.stiffeners.push_back( std::move( stiffener ) );
        if( EditStructureFeatureProperties( editFrame, next, 2, next.stiffeners.size() - 1 ) )
            editFrame->CommitBoardStructure( next, _( "Draw board-structure stiffener" ) );
    }
    else
    {
        std::string stackId;
        const auto kind = aStackKind == 1 ? BOARD_STRUCTURE_STACK_KIND::FLEX : BOARD_STRUCTURE_STACK_KIND::RIGID;
        if( !ChooseStructureStack( editFrame, next, kind, stackId ) ) return 0;
        BOARD_STRUCTURE_REGION region;
        region.id = nextId( next.regions, "region-" );
        region.name = region.id;
        region.stack_id = stackId;
        region.shape.outline = std::move( points );
        next.regions.push_back( std::move( region ) );
        if( EditStructureFeatureProperties( editFrame, next, 0, next.regions.size() - 1 ) )
            editFrame->CommitBoardStructure( next, _( "Draw board-structure region" ) );
    }
    return 0;
}

int BOARD_STRUCTURE_TOOL::DrawBend( const TOOL_EVENT& aEvent ) { return drawBend( aEvent ); }

int BOARD_STRUCTURE_TOOL::drawBend( const TOOL_EVENT& aEvent )
{
    PCB_EDIT_FRAME* editFrame = dynamic_cast<PCB_EDIT_FRAME*>( frame() );
    if( !editFrame ) return 0;
    Activate();
    controls()->ShowCursor( true );
    controls()->ForceCursorPosition( false );
    VECTOR2I start, end;
    bool gotStart = false;
    m_preview.SetPoints( {}, {}, {} );
    view()->Add( &m_preview );
    view()->SetVisible( &m_preview, true );
    m_preview.SetLineColor( KIGFX::COLOR4D( 1.0, 0.75, 0.15, 1.0 ) );
    m_preview.SetFillColor( KIGFX::COLOR4D( 1.0, 0.75, 0.15, 0.0 ) );
    auto clearPreview = [&]()
    {
        view()->SetVisible( &m_preview, false );
        view()->Update( &m_preview, KIGFX::GEOMETRY );
    };
    PCB_GRID_HELPER grid( m_toolMgr, editFrame->GetMagneticItemsSettings() );
    LSET layers;
    layers.set();
    while( TOOL_EVENT* event = Wait() )
    {
        if( event->IsCancelInteractive() || event->IsActivate() ) { clearPreview(); return 0; }
        if( event->IsClick( BUT_RIGHT ) ) { clearPreview(); return 0; }
        if( event->IsClick( BUT_LEFT ) )
        {
            VECTOR2I point = grid.ResolveSnap( eventPosition( event, controls() ), layers,
                                                  GRID_HELPER_GRIDS::GRID_GRAPHICS ).position;
            if( !gotStart ) { start = point; gotStart = true; }
            else { end = point; break; }
        }
        else if( event->IsMotion() && gotStart )
        {
            VECTOR2I point = grid.ResolveSnap( eventPosition( event, controls() ), layers,
                                                  GRID_HELPER_GRIDS::GRID_GRAPHICS ).position;
            SHAPE_LINE_CHAIN locked, leader;
            locked.Append( start );
            leader.Append( start );
            leader.Append( point );
            SHAPE_LINE_CHAIN loop;
            m_preview.SetPoints( locked, leader, loop );
            view()->Update( &m_preview, KIGFX::GEOMETRY );
        }
        else
            event->SetPassEvent();
    }
    clearPreview();
    const BOARD_STRUCTURE_REGION* region = chooseRegion( editFrame, start );
    if( !gotStart || !region || start == end ) return 0;
    BOARD_STRUCTURE next = editFrame->GetBoard()->GetBoardStructure();
    BOARD_STRUCTURE_BEND bend;
    bend.id = nextId( next.bends, "bend-" );
    bend.region_id = region->id;
    bend.start = start;
    bend.end = end;
    bend.angle_degrees = 90.0;
    bend.inner_radius_mm = 1.0;
    bend.moving_side_left = true;
    bend.sequence = static_cast<int>( next.bends.size() );
    next.bends.push_back( bend );
    if( EditStructureFeatureProperties( editFrame, next, 3, next.bends.size() - 1 ) )
        editFrame->CommitBoardStructure( next, _( "Draw board-structure bend" ) );
    return 0;
}
