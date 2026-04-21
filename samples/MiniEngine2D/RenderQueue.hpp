#pragma once

#include "RenderTypes.hpp"

#include <condition_variable>
#include <mutex>
#include <optional>

namespace mini2d
{
	class RenderQueue
	{
	public:
		void Submit( RenderFrame&& frame )
		{
			{
				std::lock_guard<std::mutex> lock( mutex_ );
				if( stopping_ )
				{
					return;
				}

				latestFrame_ = std::move( frame );
			}

			condition_.notify_one();
		}

		bool WaitAndPop( RenderFrame& frame )
		{
			std::unique_lock<std::mutex> lock( mutex_ );
			condition_.wait( lock, [this]()
				{
					return stopping_ || latestFrame_.has_value();
				} );

			if( !latestFrame_.has_value() )
			{
				return false;
			}

			frame = std::move( *latestFrame_ );
			latestFrame_.reset();
			return true;
		}

		void Stop()
		{
			{
				std::lock_guard<std::mutex> lock( mutex_ );
				stopping_ = true;
			}

			condition_.notify_all();
		}

		void Reset()
		{
			std::lock_guard<std::mutex> lock( mutex_ );
			latestFrame_.reset();
			stopping_ = false;
		}

	private:
		std::mutex mutex_;
		std::condition_variable condition_;
		std::optional<RenderFrame> latestFrame_;
		bool stopping_ = false;
	};
}
