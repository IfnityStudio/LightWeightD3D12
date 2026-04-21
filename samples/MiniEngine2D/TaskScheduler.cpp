#include "TaskScheduler.hpp"

#include "Profiler.hpp"

#include <string>
#include <stdexcept>

namespace mini2d
{
	TaskScheduler::TaskScheduler( size_t workerCount )
	{
		workers_.reserve( workerCount );
		for( size_t index = 0; index < workerCount; ++index )
		{
			workers_.emplace_back( [this, index]()
				{
					const std::string threadName = "TaskWorker " + std::to_string( index );
					SetProfilerThreadName( threadName.c_str() );
					WorkerLoop();
				} );
		}
	}

	TaskScheduler::~TaskScheduler()
	{
		Stop();
	}

	void TaskScheduler::Stop()
	{
		{
			std::lock_guard<std::mutex> lock( mutex_ );
			stopping_ = true;
		}

		condition_.notify_all();

		for( std::thread& worker : workers_ )
		{
			if( worker.joinable() )
			{
				worker.join();
			}
		}

		workers_.clear();
	}

	size_t TaskScheduler::GetWorkerCount() const noexcept
	{
		return workers_.size();
	}

	void TaskScheduler::WorkerLoop()
	{
		for( ;; )
		{
			std::function<void()> job;
			{
				std::unique_lock<std::mutex> lock( mutex_ );
				condition_.wait( lock, [this]()
					{
						return stopping_ || !jobs_.empty();
					} );

				if( stopping_ && jobs_.empty() )
				{
					return;
				}

				job = std::move( jobs_.front() );
				jobs_.pop();
			}

			MINI_PROFILE_SCOPE( "TaskScheduler::ExecuteJob" );
			job();
		}
	}
}
