/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Jon Evans <jon@craftyjon.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KICAD_BOARD_CONTEXT_H
#define KICAD_BOARD_CONTEXT_H

#include <memory>

#include <wx/string.h>

class BOARD;
class KIWAY;
class PCB_EDIT_FRAME;
class PROJECT;
class TOOL_MANAGER;


/// An interface for the frame surface that the API handlers need; to enable headless mode
class BOARD_CONTEXT
{
public:
    virtual ~BOARD_CONTEXT() = default;

    virtual BOARD* GetBoard() const = 0;

    virtual PROJECT& Prj() const = 0;

    virtual TOOL_MANAGER* GetToolManager() const = 0;

    virtual KIWAY* GetKiway() const = 0;

    virtual wxString GetCurrentFileName() const = 0;

    virtual bool CanAcceptApiCommands() const = 0;

    virtual bool SaveBoard() = 0;

    virtual bool SavePcbCopy( const wxString& aFileName, bool aCreateProject, bool aHeadless ) = 0;
};


std::shared_ptr<BOARD_CONTEXT> CreatePcbFrameContext( PCB_EDIT_FRAME* aFrame );

#endif
