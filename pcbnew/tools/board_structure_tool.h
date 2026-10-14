/* Interactive canvas authoring for the board-structure model. */
#ifndef BOARD_STRUCTURE_TOOL_H
#define BOARD_STRUCTURE_TOOL_H

#include <tools/pcb_tool_base.h>
#include <preview_items/polygon_item.h>

class BOARD_STRUCTURE_TOOL : public PCB_TOOL_BASE
{
public:
    BOARD_STRUCTURE_TOOL();
    ~BOARD_STRUCTURE_TOOL() override;

    bool Init() override;
    void Reset( RESET_REASON aReason ) override;

    int EditFeature( const TOOL_EVENT& aEvent );
    int DrawBoundary( const TOOL_EVENT& aEvent );
    int AssignStack( const TOOL_EVENT& aEvent );
    int EditBoundary( const TOOL_EVENT& aEvent );
    int DrawRegion( const TOOL_EVENT& aEvent );
    int DrawBend( const TOOL_EVENT& aEvent );
    int DrawStiffener( const TOOL_EVENT& aEvent );

private:
    void setTransitions() override;
    int drawPolygon( const TOOL_EVENT& aEvent, bool aStiffener, int aStackKind = -1 );
    int drawBoundary( const TOOL_EVENT& aEvent );
    int drawBend( const TOOL_EVENT& aEvent );

    KIGFX::PREVIEW::POLYGON_ITEM m_preview;
};

#endif
