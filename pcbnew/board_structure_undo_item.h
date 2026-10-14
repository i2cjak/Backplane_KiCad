// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <board_structure.h>
#include <eda_item.h>

// A transient undo record, not a native board item or a file-format extension.
class BOARD_STRUCTURE_UNDO_ITEM : public EDA_ITEM
{
public:
    explicit BOARD_STRUCTURE_UNDO_ITEM( const BOARD_STRUCTURE& aStructure ) :
            EDA_ITEM( TYPE_NOT_INIT ), structure( aStructure )
    {
        SetFlags( UR_TRANSIENT );
    }

    wxString GetClass() const override { return wxT( "BOARD_STRUCTURE_UNDO_ITEM" ); }
#if defined(DEBUG)
    void Show( int, std::ostream& ) const override {}
#endif
    BOARD_STRUCTURE structure;
};
