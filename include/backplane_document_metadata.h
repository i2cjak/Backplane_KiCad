/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <nlohmann/json.hpp>
#include <wx/string.h>

class EDA_ITEM;
class LINE_ENDING;

/**
 * Extra IPC data that cannot be represented in stock KiCad 10.0.6 files.
 *
 * Native geometry and library definitions remain in the normal KiCad file.  This
 * companion only stores additions, so edits made in stock KiCad remain authoritative.
 * Callers use item UUIDs, or stable parent/field names for objects without saved UUIDs.
 */
class BACKPLANE_DOCUMENT_METADATA
{
public:
    void Load( const wxString& aDocumentPath );
    void Save( const wxString& aDocumentPath ) const;

    void Capture( const wxString& aKey, const EDA_ITEM& aItem );
    void Apply( const wxString& aKey, EDA_ITEM& aItem ) const;

    // SCH_LINE has line endings but does not inherit EDA_SHAPE.
    void CaptureLineEndings( const wxString& aKey, const LINE_ENDING& aStart,
                             const LINE_ENDING& aEnd );
    bool ReadLineEndings( const wxString& aKey, LINE_ENDING& aStart,
                          LINE_ENDING& aEnd ) const;

    // Domain-specific additions that stock 10.0.6 cannot represent.
    void CaptureExtension( const wxString& aKey, const std::string& aName,
                           const nlohmann::json& aData );
    nlohmann::json ReadExtension( const wxString& aKey, const std::string& aName ) const;

    static wxString CompanionPath( const wxString& aDocumentPath );

private:
    nlohmann::json m_items = nlohmann::json::object();
};
