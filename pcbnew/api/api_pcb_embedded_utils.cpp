/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <api/api_pcb_embedded_utils.h>
#include <api/api_enums.h>
#include <embedded_files.h>

namespace kiapi::board
{
void PackEmbeddedFiles( common::types::EmbeddedFiles& aOutput, const EMBEDDED_FILES& aFiles )
{
    aOutput.clear_files();

    for( const auto& [name, file] : aFiles.EmbeddedFileMap() )
    {
        if( file->compressedEncodedData.empty() )
            continue;

        common::types::EmbeddedFile* proto = aOutput.add_files();
        proto->set_name( name.ToUTF8() );
        proto->set_type( ToProtoEnum<EMBEDDED_FILES::EMBEDDED_FILE::FILE_TYPE,
                                    common::types::EmbeddedFileType>( file->type ) );
        proto->set_data( file->compressedEncodedData );
        proto->set_data_hash( file->data_hash );
    }
}


bool UnpackEmbeddedFiles( EMBEDDED_FILES& aOutput, const common::types::EmbeddedFiles& aProto )
{
    // Validate the complete replacement before touching the document.
    EMBEDDED_FILES decoded;

    for( const common::types::EmbeddedFile& proto : aProto.files() )
    {
        auto file = std::make_shared<EMBEDDED_FILES::EMBEDDED_FILE>();
        file->name = wxString::FromUTF8( proto.name() );
        file->type = FromProtoEnum<EMBEDDED_FILES::EMBEDDED_FILE::FILE_TYPE>( proto.type() );
        file->compressedEncodedData = proto.data();
        file->data_hash = proto.data_hash();

        if( file->name.empty() || decoded.GetEmbeddedFile( file->name )
            || EMBEDDED_FILES::DecompressAndDecode( *file ) != EMBEDDED_FILES::RETURN_CODE::OK
            || !file->Validate() )
        {
            return false;
        }

        decoded.AddFile( file );
    }

    aOutput.ClearEmbeddedFiles();

    for( const auto& [name, file] : decoded.EmbeddedFileMap() )
        aOutput.AddFile( file );

    return true;
}
}
