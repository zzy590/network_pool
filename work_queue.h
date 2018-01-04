/* Copyright (c) 2018 Zhenyu Zhang. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace NETWORK_POOL
{
	class Ctask
	{
	public:
		virtual ~Ctask() {}

		virtual void run() = 0;
	};

	class CworkQueue
	{
	private:
		std::vector<std::thread> m_threads;

		std::mutex m_lock;
		bool m_exit;
		std::condition_variable m_cv;
		std::deque<Ctask *> m_tasks;

		Ctask *getTask()
		{
			std::unique_lock<std::mutex> lck(m_lock);
			while (!m_exit)
			{
				if (m_tasks.empty())
					m_cv.wait(lck);
				else
				{
					Ctask *task = m_tasks.front();
					m_tasks.pop_front();
					return task;
				}
			}
			return nullptr;
		}

		void worker()
		{
			while (true)
			{
				Ctask *task = getTask();
				if (nullptr == task)
					break;
				task->run();
				delete task;
			}
		}

		void setExit()
		{
			std::unique_lock<std::mutex> lck(m_lock);
			m_exit = true;
			m_cv.notify_all();
		}

	public:
		CworkQueue(const size_t nThread)
			:m_exit(false)
		{
			try
			{
				for (size_t i = 0; i < nThread; ++i)
					m_threads.push_back(std::move(std::thread(&CworkQueue::worker, this)));
			}
			catch (...)
			{
				setExit();
				for (auto& thread : m_threads)
					thread.join();
				throw;
			}
		}
		~CworkQueue()
		{
			setExit();
			for (auto& thread : m_threads)
				thread.join();
			for (auto& task : m_tasks)
				delete task;
		}

		// No copy, no move.
		CworkQueue(const CworkQueue& another) = delete;
		CworkQueue(CworkQueue&& another) = delete;
		const CworkQueue& operator=(const CworkQueue& another) = delete;
		const CworkQueue& operator=(CworkQueue&& another) = delete;

		void pushTask(Ctask *task)
		{
			std::unique_lock<std::mutex> lck(m_lock);
			m_tasks.push_back(task);
			m_cv.notify_one();
		}
	};
}
