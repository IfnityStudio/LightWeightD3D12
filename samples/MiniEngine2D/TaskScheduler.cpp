#include "TaskScheduler.hpp"

namespace mini2d
{
	TaskScheduler::TaskScheduler( size_t workerCount )
	{
		const size_t resolvedWorkerCount = std::max<size_t>( 1, workerCount );
		workers_.reserve( resolvedWorkerCount );
		for( size_t index = 0; index < resolvedWorkerCount; ++index )
		{
			workers_.emplace_back( [this]()
				{
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
			if( stopping_ )
			{
				return;
			}

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

			job();
		}
	}
}
