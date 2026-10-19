/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <json_common.h>
#include <wx/string.h>

class EDA_ITEM;
class LINE_ENDING;

/**
 * Fork-only document extensions and read-only migration of legacy v1 items.
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
    nlohmann::json ToJson() const;

    // History needs an empty wrapper after the last extension is removed, so
    // restoring an older companion cannot resurrect deleted construction.
    bool HasExtensionHistory() const { return m_hadExtensions; }

    void Apply( const wxString& aKey, EDA_ITEM& aItem ) const;

    // SCH_LINE has line endings but does not inherit EDA_SHAPE.
    bool ReadLineEndings( const wxString& aKey, LINE_ENDING& aStart,
                          LINE_ENDING& aEnd ) const;

    // Domain-specific additions that stock 10.0.6 cannot represent.
    void CaptureExtension( const wxString& aKey, const std::string& aName,
                           const nlohmann::json& aData );
    nlohmann::json ReadExtension( const wxString& aKey, const std::string& aName ) const;

    static wxString CompanionPath( const wxString& aDocumentPath );

private:
    bool m_hadExtensions = false;
    nlohmann::json m_extensions = nlohmann::json::object();
    nlohmann::json m_items = nlohmann::json::object();
};
