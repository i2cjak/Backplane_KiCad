/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "dialog_structure_stack_preset.h"

#include <cmath>
#include <algorithm>
#include <vector>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <pcb_edit_frame.h>

namespace
{
class STACK_PRESET_DIALOG : public wxDialog
{
public:
    STACK_PRESET_DIALOG( PCB_EDIT_FRAME* aParent, BOARD_STRUCTURE& aStaged,
                         BOARD_STRUCTURE_STACK_KIND aKind, bool aForceNew ) :
            wxDialog( aParent, wxID_ANY, _( "Structure Stack" ), wxDefaultPosition, wxDefaultSize,
                      wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ),
            m_staged( aStaged ),
            m_kind( aKind )
    {
        const bool flex = aKind == BOARD_STRUCTURE_STACK_KIND::FLEX;
        auto* outer = new wxBoxSizer( wxVERTICAL );

        const auto matching = [&]()
        {
            for( const auto& stack : aStaged.stacks )
                if( stack.kind == aKind )
                    m_matching.push_back( &stack );
        };
        if( !aForceNew )
            matching();

        if( !m_matching.empty() )
        {
            outer->Add( new wxStaticText( this, wxID_ANY,
                                          _( "Use an existing structure stack:" ) ), 0,
                        wxEXPAND | wxALL, 8 );
            m_existing = new wxChoice( this, wxID_ANY );
            for( const auto* stack : m_matching )
                m_existing->Append( wxString::FromUTF8( stack->name ),
                                    const_cast<BOARD_STRUCTURE_STACK*>( stack ) );
            m_existing->SetSelection( 0 );
            outer->Add( m_existing, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8 );
        }
        else
        {
            outer->Add( new wxStaticText( this, wxID_ANY,
                                          flex ? _( "Create a flex starter stack for this board." )
                                               : _( "Create a rigid starter stack for this board." ) ),
                        0, wxEXPAND | wxALL, 8 );
            auto* form = new wxFlexGridSizer( 0, 2, 5, 8 );
            form->AddGrowableCol( 1 );
            form->Add( new wxStaticText( this, wxID_ANY, _( "Stack name" ) ), 0,
                       wxALIGN_CENTER_VERTICAL );
            m_name = new wxTextCtrl( this, wxID_ANY, flex ? _( "Flex starter" )
                                                            : _( "Rigid FR4 starter" ) );
            form->Add( m_name, 1, wxEXPAND );
            form->Add( new wxStaticText( this, wxID_ANY, _( "Copper pair" ) ), 0,
                       wxALIGN_CENTER_VERTICAL );
            m_copper = new wxChoice( this, wxID_ANY );
            m_copper->Append( _( "F.Cu / B.Cu" ) );
            m_copper->SetSelection( 0 );
            form->Add( m_copper, 1, wxEXPAND );
            form->Add( new wxStaticText( this, wxID_ANY, flex ? _( "Flex dielectric (mm)" )
                                                                : _( "Rigid dielectric (mm)" ) ), 0,
                       wxALIGN_CENTER_VERTICAL );
            m_dielectric = new wxSpinCtrlDouble( this, wxID_ANY );
            m_dielectric->SetRange( 0.001, 10.0 );
            m_dielectric->SetIncrement( 0.01 );
            m_dielectric->SetValue( flex ? 0.05 : 1.53 );
            m_dielectric->SetDigits( 3 );
            form->Add( m_dielectric, 1, wxEXPAND );
            form->Add( new wxStaticText( this, wxID_ANY, _( "Copper thickness (mm)" ) ), 0,
                       wxALIGN_CENTER_VERTICAL );
            m_copperThickness = new wxSpinCtrlDouble( this, wxID_ANY );
            m_copperThickness->SetRange( 0.001, 1.0 );
            m_copperThickness->SetIncrement( 0.001 );
            m_copperThickness->SetValue( flex ? 0.018 : 0.035 );
            m_copperThickness->SetDigits( 3 );
            form->Add( m_copperThickness, 1, wxEXPAND );
            outer->Add( form, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8 );
        }

        auto* buttons = new wxStdDialogButtonSizer;
        buttons->AddButton( new wxButton( this, wxID_OK ) );
        buttons->AddButton( new wxButton( this, wxID_CANCEL ) );
        buttons->Realize();
        outer->Add( buttons, 0, wxEXPAND | wxALL, 8 );
        SetSizerAndFit( outer );
        CentreOnParent();
    }

    bool Transfer( std::string& aId )
    {
        if( m_existing )
        {
            auto* selected = static_cast<BOARD_STRUCTURE_STACK*>(
                    m_existing->GetClientData( m_existing->GetSelection() ) );
            if( !selected )
                return false;
            aId = selected->id;
            return true;
        }

        if( !m_name || m_name->GetValue().Trim().empty() || m_dielectric->GetValue() <= 0
            || m_copperThickness->GetValue() <= 0 )
            return false;

        const bool flex = m_kind == BOARD_STRUCTURE_STACK_KIND::FLEX;
        BOARD_STRUCTURE_STACK stack;
        stack.id = m_kind == BOARD_STRUCTURE_STACK_KIND::FLEX ? "flex-starter" : "rigid-starter";
        int suffix = 1;
        while( std::any_of( m_staged.stacks.begin(), m_staged.stacks.end(),
                            [&]( const auto& s ) { return s.id == stack.id; } ) )
            stack.id = ( m_kind == BOARD_STRUCTURE_STACK_KIND::FLEX ? "flex-starter-" : "rigid-starter-" )
                       + std::to_string( suffix++ );
        stack.name = m_name->GetValue().ToStdString();
        stack.kind = m_kind;
        stack.copper_layers = { B_Cu, F_Cu };
        const int64_t copper = static_cast<int64_t>( std::llround( m_copperThickness->GetValue() * 1e6 ) );
        const int64_t dielectric = static_cast<int64_t>( std::llround( m_dielectric->GetValue() * 1e6 ) );
        const int64_t total = copper * 2 + dielectric;
        BOARD_STRUCTURE_MATERIAL bottomCu{ "cu-bottom", "copper", "Copper", copper };
        BOARD_STRUCTURE_MATERIAL core{ flex ? "flex-core" : "rigid-core", "dielectric",
                                       flex ? "Polyimide" : "FR-4", dielectric };
        BOARD_STRUCTURE_MATERIAL topCu{ "cu-top", "copper", "Copper", copper };
        stack.materials = { bottomCu, core, topCu };
        stack.layers = { { bottomCu.id, B_Cu, -total / 2 },
                          { core.id, -1, -total / 2 + copper },
                          { topCu.id, F_Cu, -total / 2 + copper + dielectric } };
        m_staged.stacks.push_back( std::move( stack ) );
        aId = m_staged.stacks.back().id;
        return true;
    }

private:
    BOARD_STRUCTURE& m_staged;
    BOARD_STRUCTURE_STACK_KIND m_kind;
    std::vector<const BOARD_STRUCTURE_STACK*> m_matching;
    wxChoice* m_existing = nullptr;
    wxTextCtrl* m_name = nullptr;
    wxChoice* m_copper = nullptr;
    wxSpinCtrlDouble* m_dielectric = nullptr;
    wxSpinCtrlDouble* m_copperThickness = nullptr;
};
}

bool ChooseStructureStack( PCB_EDIT_FRAME* aParent, BOARD_STRUCTURE& aStaged,
                           BOARD_STRUCTURE_STACK_KIND aKind, std::string& aOutputStackId,
                           bool aForceNew )
{
    BOARD_STRUCTURE working = aStaged;
    STACK_PRESET_DIALOG dialog( aParent, working, aKind, aForceNew );
    if( dialog.ShowModal() != wxID_OK )
        return false;

    if( !dialog.Transfer( aOutputStackId ) )
    {
        wxMessageBox( _( "Enter a stack name and positive dielectric and copper thicknesses.\n"
                        "The starter uses a centered F.Cu/B.Cu pair; review its materials and "
                        "thickness before committing the board structure." ),
                      _( "Invalid Structure Stack" ), wxOK | wxICON_WARNING, aParent );
        return false;
    }
    aStaged = std::move( working );
    return true;
}
