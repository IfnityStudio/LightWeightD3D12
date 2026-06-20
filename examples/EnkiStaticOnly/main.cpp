#include "TaskScheduler.h"

#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    std::mutex gLogMutex;
    std::vector<std::string> gLog;

    void PushLog( const std::string& text )
    {
        std::lock_guard<std::mutex> lock( gLogMutex );
        gLog.push_back( text );
    }

    class ManagerTask final : public enki::ITaskSet
    {
    public:
        ManagerTask( std::string name, uint32_t workItems, uint32_t minRange ):
            enki::ITaskSet( workItems, minRange ),
            name_( std::move( name ) )
        {
        }

        void ExecuteRange( enki::TaskSetPartition range, uint32_t threadnum ) override
        {
            uint64_t checksum = 0;
            for( uint32_t index = range.start; index < range.end; ++index )
            {
                checksum += static_cast<uint64_t>( index + 1u ) * 17u;
            }

            PushLog(
                name_ +
                " ran on thread " + std::to_string( threadnum ) +
                " with range [" + std::to_string( range.start ) +
                ", " + std::to_string( range.end ) +
                ") checksum=" + std::to_string( checksum ) );
        }

    private:
        std::string name_;
    };
}

int main()
{
    enki::TaskScheduler scheduler;
    scheduler.Initialize();

    ManagerTask fileReviewTask( "FileReviewManager", 4096, 256 );
    ManagerTask validationTask( "ValidationManager", 3072, 256 );
    ManagerTask errorCheckTask( "ErrorCheckManager", 2048, 256 );

    scheduler.AddTaskSetToPipe( &fileReviewTask );
    scheduler.AddTaskSetToPipe( &validationTask );
    scheduler.AddTaskSetToPipe( &errorCheckTask );

    scheduler.WaitforAll();

    std::cout << "All manager tasks finished. Render can continue.\n\n";
    for( const std::string& line : gLog )
    {
        std::cout << line << '\n';
    }

    scheduler.WaitforAllAndShutdown();
    return 0;
}
