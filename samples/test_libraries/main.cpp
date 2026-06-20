#include <LightD3D12/LightD3D12.hpp>
#include "TaskScheduler.h"

#include <cstdint>
#include <iostream>

namespace
{
    class ExampleTask final : public enki::ITaskSet
    {
    public:
        ExampleTask(): enki::ITaskSet( 1024, 128 ) {}

        void ExecuteRange( enki::TaskSetPartition range, uint32_t threadnum ) override
        {
            uint64_t checksum = 0;
            for( uint32_t index = range.start; index < range.end; ++index )
            {
                checksum += static_cast<uint64_t>( index + 1u ) * 17u;
            }

            std::cout << "Task range [" << range.start << ", " << range.end
                      << ") on thread " << threadnum
                      << ", checksum=" << checksum << '\n';
        }
    };
}
int main()
{
    std::cout << "test_libraries running.\\n";
    lightd3d12::SubmitHandle emptySubmitHandle{};
    (void)emptySubmitHandle;

    enki::TaskScheduler scheduler;
    scheduler.Initialize();

    ExampleTask task;
    scheduler.AddTaskSetToPipe( &task );
    scheduler.WaitforAll();
    scheduler.WaitforAllAndShutdown();
    return 0;
}
