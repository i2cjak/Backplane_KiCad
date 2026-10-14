/* Render-only view item for the fork's board structure companion data. */
#pragma once

#include <array>
#include <eda_item.h>

class BOARD;

class BOARD_STRUCTURE_VIEW_ITEM : public EDA_ITEM
{
public:
    enum CATEGORY { RIGID = 0, FLEX, STIFFENER, BEND, BOUNDARY, F_STIFFENER, B_STIFFENER, CATEGORY_COUNT };
    explicit BOARD_STRUCTURE_VIEW_ITEM( BOARD* aBoard );
    const BOX2I ViewBBox() const override;
    void ViewDraw( int aLayer, KIGFX::VIEW* aView ) const override;
    std::vector<int> ViewGetLayers() const override;
    void SetVisible( int aCategory, bool aVisible );
    bool IsVisible( int aCategory ) const;
    wxString GetClass() const override { return wxT( "BOARD_STRUCTURE_VIEW_ITEM" ); }
    bool HitTest( const VECTOR2I&, int = 0 ) const override { return false; }

private:
    BOARD* m_board;
    std::array<bool, CATEGORY_COUNT> m_visible{ true, true, true, true, true, true, true };
};
