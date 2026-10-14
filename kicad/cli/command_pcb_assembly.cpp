#include "command_pcb_assembly.h"
#include <jobs/job_pcb_assembly.h>
#include <cli/exit_codes.h>
#include <wx/file.h>
CLI::PCB_ASSEMBLY_COMMAND::PCB_ASSEMBLY_COMMAND() : COMMAND( "assembly" )
{
    m_argParser.add_description( "Export or check a multi-board assembly manifest" );
    addCommonArgs( true, true, IO_TYPE::FILE, IO_TYPE::FILE );
    m_argParser.add_argument( "--check" ).flag().help( "Check assembly collisions" );
}
int CLI::PCB_ASSEMBLY_COMMAND::doPerform( KIWAY& kiway )
{
    JOB_PCB_ASSEMBLY job; job.filename = m_argInput; job.output = m_argOutput; job.check = m_argParser.get<bool>( "--check" );
    if( !wxFile::Exists( job.filename ) || ( !job.check && job.output.IsEmpty() ) ) return EXIT_CODES::ERR_ARGS;
    job.SetConfiguredOutputPath( job.output ); return kiway.ProcessJob( KIWAY::FACE_PCB, &job );
}
