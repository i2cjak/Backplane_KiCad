/*
 * This program source code file is part of KiCad, a free EDA application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <wx/panel.h>
#include <wx/arrstr.h>
#include <vector>
#include <board_structure.h>

class BOARD;
class PCB_EDIT_FRAME;
class wxChoice;
class wxGrid;
class wxGridEvent;
class wxListBox;
class wxNotebook;
class wxSpinCtrlDouble;
class wxTextCtrl;

/** A deliberately small presentation record.  The board structure owns the
 * authoritative objects; this record keeps this panel usable while a board is
 * being upgraded and is also useful to import/export adapters. */
struct RIGID_FLEX_UI_RECORD
{
    wxString name;
    wxString type;
    wxString stack;
    wxString geometry;
    wxString details;
    wxString extra;
};

class PANEL_SETUP_RIGID_FLEX : public wxPanel
{
public:
    PANEL_SETUP_RIGID_FLEX( wxWindow* aParent, PCB_EDIT_FRAME* aFrame );

    // Called by the board-structure adapter when the companion is loaded.
    void LoadRecords( const std::vector<RIGID_FLEX_UI_RECORD>& aRegions,
                      const std::vector<RIGID_FLEX_UI_RECORD>& aStacks,
                      const std::vector<RIGID_FLEX_UI_RECORD>& aStiffeners,
                      const std::vector<RIGID_FLEX_UI_RECORD>& aCoverlays,
                      const std::vector<RIGID_FLEX_UI_RECORD>& aBends );

    void GetRecords( std::vector<RIGID_FLEX_UI_RECORD>& aRegions,
                     std::vector<RIGID_FLEX_UI_RECORD>& aStacks,
                     std::vector<RIGID_FLEX_UI_RECORD>& aStiffeners,
                     std::vector<RIGID_FLEX_UI_RECORD>& aCoverlays,
                     std::vector<RIGID_FLEX_UI_RECORD>& aBends ) const;

    // Returns false when one of the entered values is structurally invalid.
    bool Validate( wxString* aError = nullptr ) const;
    bool TransferDataFromWindow() override;

    // Commit the staged structure after the containing Board Setup dialog has
    // accepted the complete set of pages.  TransferDataFromWindow() only
    // stages values so navigating between pages and pressing Cancel is safe.
    bool ApplyChanges();

private:
    void BuildRecordPage( wxNotebook* aBook, const wxString& aTitle, wxGrid*& aGrid,
                          const wxArrayString& aColumns );
    void OnAdd( wxCommandEvent& aEvent );
    void OnDelete( wxCommandEvent& aEvent );
    void OnCellChanged( wxGridEvent& aEvent );
    void EditSelectedStackLayers();
    void MarkChanged();
    void OnPreviewPaint( wxPaintEvent& aEvent );
    wxGrid* CurrentGrid() const;
    std::vector<RIGID_FLEX_UI_RECORD>& CurrentRecords();
    const std::vector<RIGID_FLEX_UI_RECORD>& CurrentRecords() const;

    PCB_EDIT_FRAME* m_frame;
    wxNotebook* m_book;
    wxGrid* m_regions;
    wxGrid* m_stacks;
    wxGrid* m_stiffeners;
    wxGrid* m_coverlays;
    wxGrid* m_bends;
    wxTextCtrl* m_validation;
    wxPanel* m_preview;
    BOARD* m_board;
    BOARD_STRUCTURE m_stagedStructure;
    std::vector<RIGID_FLEX_UI_RECORD> m_regionData;
    std::vector<RIGID_FLEX_UI_RECORD> m_stackData;
    std::vector<RIGID_FLEX_UI_RECORD> m_stiffenerData;
    std::vector<RIGID_FLEX_UI_RECORD> m_coverlayData;
    std::vector<RIGID_FLEX_UI_RECORD> m_bendData;
};
