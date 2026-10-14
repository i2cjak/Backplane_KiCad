// SPDX-License-Identifier: GPL-3.0-or-later
#include "dialog_structure_properties.h"
#include <board_structure.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <algorithm>
#include <cmath>

bool EditStructureFeatureProperties( wxWindow* parent, BOARD_STRUCTURE& structure,
                                     int category, size_t index )
{
    wxDialog dialog( parent, wxID_ANY, category == 0 ? _( "Region Properties" ) :
                     category == 2 ? _( "Stiffener Properties" ) : _( "Bend Properties" ) );
    auto* root = new wxBoxSizer( wxVERTICAL );
    auto* grid = new wxFlexGridSizer( 2, 8, 12 );
    grid->AddGrowableCol( 1 );
    auto field = [&]( const wxString& label, const wxString& value )
    {
        grid->Add( new wxStaticText( &dialog, wxID_ANY, label ), 0, wxALIGN_CENTER_VERTICAL );
        auto* text = new wxTextCtrl( &dialog, wxID_ANY, value, wxDefaultPosition, wxSize( 240, -1 ) );
        grid->Add( text, 1, wxEXPAND );
        return text;
    };
    auto choice = [&]( const wxString& label, const wxArrayString& values, int selected )
    {
        grid->Add( new wxStaticText( &dialog, wxID_ANY, label ), 0, wxALIGN_CENTER_VERTICAL );
        auto* control = new wxChoice( &dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, values );
        control->SetSelection( selected );
        grid->Add( control, 1, wxEXPAND );
        return control;
    };
    auto number = []( double v ) { return wxString::Format( "%.4f", v ); };
    wxTextCtrl *name = nullptr, *material = nullptr, *adhesive = nullptr,
               *thickness = nullptr, *adhesiveThickness = nullptr,
               *angle = nullptr, *radius = nullptr, *sequence = nullptr;
    wxChoice *stack = nullptr, *side = nullptr, *region = nullptr;
    std::vector<std::string> stackIds, regionIds;
    if( category == 0 )
    {
        const auto& item = structure.regions.at( index );
        name = field( _( "Name" ), wxString::FromUTF8( item.name ) );
        wxArrayString names;
        int selected = wxNOT_FOUND;
        for( const auto& s : structure.stacks )
        {
            if( s.id == item.stack_id ) selected = names.size();
            names.Add( wxString::FromUTF8( s.name ) +
                       ( s.kind == BOARD_STRUCTURE_STACK_KIND::FLEX ? " (Flex)" : " (Rigid)" ) );
            stackIds.push_back( s.id );
        }
        stack = choice( _( "Regional stack" ), names, selected );
    }
    else
    {
        const std::string current = category == 2 ? structure.stiffeners.at( index ).region_id :
                                                   structure.bends.at( index ).region_id;
        wxArrayString names;
        int selected = wxNOT_FOUND;
        for( const auto& r : structure.regions )
        {
            const auto* s = structure.StackForRegion( r );
            if( category == 3 && ( !s || s->kind != BOARD_STRUCTURE_STACK_KIND::FLEX ) ) continue;
            if( r.id == current ) selected = names.size();
            names.Add( wxString::FromUTF8( r.name.empty() ? r.id : r.name ) );
            regionIds.push_back( r.id );
        }
        region = choice( _( "Region" ), names, selected );
        if( category == 2 )
        {
            const auto& s = structure.stiffeners.at( index );
            material = field( _( "Material" ), wxString::FromUTF8( s.material ) );
            thickness = field( _( "Thickness (mm)" ), number( s.thickness_nm / 1e6 ) );
            adhesive = field( _( "Adhesive" ), wxString::FromUTF8( s.adhesive ) );
            adhesiveThickness = field( _( "Adhesive thickness (mm)" ), number( s.adhesive_thickness_nm / 1e6 ) );
            side = choice( _( "Attachment side" ), { _( "Top" ), _( "Bottom" ) }, s.top ? 0 : 1 );
        }
        else
        {
            const auto& b = structure.bends.at( index );
            angle = field( _( "Angle (degrees)" ), number( b.angle_degrees ) );
            radius = field( _( "Inner radius (mm)" ), number( b.inner_radius_mm ) );
            sequence = field( _( "Fold order" ), wxString::Format( "%d", b.sequence ) );
            side = choice( _( "Moving side of axis" ), { _( "Left" ), _( "Right" ) }, b.moving_side_left ? 0 : 1 );
        }
    }
    root->Add( grid, 1, wxEXPAND | wxALL, 12 );
    auto* buttons = new wxBoxSizer( wxHORIZONTAL );
    auto* remove = new wxButton( &dialog, wxID_ANY, _( "Delete feature…" ) );
    buttons->Add( remove, 0, wxRIGHT, 12 );
    buttons->AddStretchSpacer();
    buttons->Add( dialog.CreateButtonSizer( wxOK | wxCANCEL ), 0 );
    root->Add( buttons, 0, wxEXPAND | wxALL, 12 );
    dialog.SetSizerAndFit( root );
    dialog.CentreOnParent();
    if( name ) { name->SetFocus(); name->SelectAll(); }
    if( category == 0 && !structure.boundaries.empty() )
    {
        remove->Disable();
        remove->SetToolTip( _( "Regions are defined by stackup boundaries. Edit or remove a boundary to change the partition." ) );
    }
    bool changed = false;
    remove->Bind( wxEVT_BUTTON, [&]( wxCommandEvent& )
    {
        if( wxMessageBox( category == 0 ? _( "Delete this region and its bends, stiffeners, and coverlays?" ) :
                          _( "Delete this feature?" ), _( "Delete Structure Feature" ),
                          wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, &dialog ) != wxYES ) return;
        if( category == 0 )
        {
            const auto id = structure.regions.at( index ).id;
            auto dependent = [&]( const auto& item ) { return item.region_id == id; };
            std::erase_if( structure.bends, dependent );
            std::erase_if( structure.stiffeners, dependent );
            std::erase_if( structure.coverlays, dependent );
            structure.regions.erase( structure.regions.begin() + index );
            std::erase_if( structure.region_seeds, [&]( const auto& seed ) { return seed.region_id == id; } );
        }
        else if( category == 2 ) structure.stiffeners.erase( structure.stiffeners.begin() + index );
        else structure.bends.erase( structure.bends.begin() + index );
        changed = true;
        dialog.EndModal( wxID_OK );
    } );
    dialog.Bind( wxEVT_BUTTON, [&]( wxCommandEvent& )
    {
        BOARD_STRUCTURE next = structure;
        auto fail = [&]( const wxString& text )
        { wxMessageBox( text, _( "Invalid Structure Properties" ), wxOK | wxICON_ERROR, &dialog ); };
        auto numeric = []( wxTextCtrl* field, double& value )
        { return field->GetValue().ToDouble( &value ) && std::isfinite( value ); };
        if( category == 0 )
        {
            if( stack->GetSelection() == wxNOT_FOUND || name->GetValue().Strip().empty() )
            { fail( _( "Enter a name and choose a regional stack." ) ); return; }
            auto& r = next.regions.at( index );
            r.name = name->GetValue().ToStdString();
            r.stack_id = stackIds.at( stack->GetSelection() );
        }
        else
        {
            if( region->GetSelection() == wxNOT_FOUND )
            { fail( _( "Choose a region. Bends require a flex region." ) ); return; }
            if( category == 2 )
            {
                double t, a;
                if( !numeric( thickness, t ) || !numeric( adhesiveThickness, a ) ||
                    t <= 0 || t > 100 || a < 0 || a > 100 || material->GetValue().Strip().empty() )
                { fail( _( "Enter a material and positive thickness; adhesive thickness cannot be negative." ) ); return; }
                auto& s = next.stiffeners.at( index );
                s.region_id = regionIds.at( region->GetSelection() );
                s.material = material->GetValue().ToStdString();
                s.adhesive = adhesive->GetValue().ToStdString();
                const auto newThickness = static_cast<int64_t>( std::llround( t * 1e6 ) );
                if( newThickness != s.thickness_nm )
                    s.min_thickness_nm = s.max_thickness_nm = newThickness;
                s.thickness_nm = newThickness;
                s.adhesive_thickness_nm = std::llround( a * 1e6 );
                s.top = side->GetSelection() == 0;
            }
            else
            {
                double a, r;
                long order;
                if( !numeric( angle, a ) || !numeric( radius, r ) || r <= 0 || r > 10000 ||
                    std::abs( a ) > 360 || !sequence->GetValue().ToLong( &order ) || order < 0 || order > 100000 )
                { fail( _( "Enter an angle from -360 to 360 degrees, a positive radius, and a nonnegative fold order." ) ); return; }
                auto& b = next.bends.at( index );
                b.region_id = regionIds.at( region->GetSelection() );
                b.angle_degrees = a; b.inner_radius_mm = r; b.sequence = order;
                b.moving_side_left = side->GetSelection() == 0;
            }
        }
        const auto validation = next.Validate();
        if( !validation.IsValid() )
        { fail( wxString::FromUTF8( validation.errors.front() ) ); return; }
        structure = std::move( next );
        changed = true;
        dialog.EndModal( wxID_OK );
    }, wxID_OK );
    dialog.ShowModal();
    return changed;
}
