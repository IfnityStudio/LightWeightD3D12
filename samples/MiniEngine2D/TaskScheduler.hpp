#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mini2d
{
	class TaskScheduler
	{
	public:
		explicit TaskScheduler( size_t workerCount = std::max<size_t>( 1, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1 ) );
		~TaskScheduler();

		TaskScheduler( const TaskScheduler& ) = delete;
		TaskScheduler& operator=( const TaskScheduler& ) = delete;

		template<typename Callable>
		auto Submit( Callable&& callable ) -> std::future<typename std::invoke_result_t<Callable>>
		{
			using ResultType = typename std::invoke_result_t<Callable>;

			auto task = std::make_shared<std::packaged_task<ResultType()>>( std::forward<Callable>( callable ) );
			std::future<ResultType> future = task->get_future();

			{
				std::lock_guard<std::mutex> lock( mutex_ );
				if( stopping_ )
				{
					throw std::runtime_error( "TaskScheduler is stopping and cannot accept more work." );
				}

				jobs_.push( [task]()
					{
						( *task )();
					} );
			}

			condition_.notify_one();
			return future;
		}

		void Stop();
		size_t GetWorkerCount() const noexcept;

	private:
		void WorkerLoop();

		std::vector<std::thread> workers_ = {};
		std::queue<std::function<void()>> jobs_ = {};
		std::mutex mutex_;
		std::condition_variable condition_;
		bool stopping_ = false;
	};
}
