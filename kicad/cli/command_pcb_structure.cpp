/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "command_pcb_structure.h"
#include <jobs/job_pcb_structure.h>
#include <cli/exit_codes.h>
#include <wx/file.h>

CLI::PCB_STRUCTURE_COMMAND::PCB_STRUCTURE_COMMAND() : COMMAND( "structure" )
{
    m_argParser.add_description( "Validate regional stackups, stiffeners, coverlay and bends" );
    addCommonArgs( true, true, IO_TYPE::FILE, IO_TYPE::DIRECTORY );
    m_argParser.add_argument( "--export-fabrication" ).flag()
            .help( "Export flat material Gerbers and a construction manifest to --output" );
}

int CLI::PCB_STRUCTURE_COMMAND::doPerform( KIWAY& aKiway )
{
    JOB_PCB_STRUCTURE job;
    job.filename = m_argInput;
    job.exportFabrication = m_argParser.get<bool>( "--export-fabrication" );
    if( !wxFile::Exists( job.filename ) )
        return EXIT_CODES::ERR_INVALID_INPUT_FILE;
    if( job.exportFabrication && m_argOutput.empty() )
        return EXIT_CODES::ERR_ARGS;
    job.SetConfiguredOutputPath( m_argOutput );
    return aKiway.ProcessJob( KIWAY::FACE_PCB, &job );
}
