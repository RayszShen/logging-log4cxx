/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "log4cxx/helpers/threadutility.h"
#if !defined(LOG4CXX)
	#define LOG4CXX 1
#endif
#include "log4cxx/private/log4cxx_private.h"
#include "log4cxx/helpers/loglog.h"
#include "log4cxx/helpers/transcoder.h"

#include <signal.h>
#include <mutex>
#include <list>
#include <condition_variable>
#include <algorithm>

#ifdef _WIN32
	#include <windows.h>
	#include <processthreadsapi.h>
#endif

#if LOG4CXX_EVENTS_AT_EXIT
#include <log4cxx/private/atexitregistry.h>
#endif
#if !defined(LOG4CXX)
	#define LOG4CXX 1
#endif
#include <log4cxx/helpers/aprinitializer.h>

namespace LOG4CXX_NS
{
namespace helpers
{

struct ThreadUtility::priv_data
{
	priv_data()
#if LOG4CXX_EVENTS_AT_EXIT
		: atExitRegistryRaii{ [this]{ stopThread(); } }
#endif
	{
	}

	~priv_data()
	{ stopThread(); }

	ThreadStartPre  start_pre{nullptr};
	ThreadStarted   started{nullptr};
	ThreadStartPost start_post{nullptr};

	using TimePoint = std::chrono::time_point<std::chrono::system_clock>;
	struct NamedPeriodicFunction
	{
		LogString             name;
		Period                delay;
		TimePoint             nextRun;
		std::function<void()> f;
		int                   errorCount;
		bool                  removed;
	};
	using JobStore = std::list<NamedPeriodicFunction>;
	JobStore                  jobs;
	std::recursive_mutex      job_mutex;
	std::thread               thread;
	std::condition_variable   interrupt;
	std::mutex                interrupt_mutex;
	bool                      terminated{ false };
	int                       retryCount{ 2 };
	Period                    maxDelay{ 0 };

	void doPeriodicTasks();

	void setTerminated()
	{
		std::lock_guard<std::mutex> lock(interrupt_mutex);
		terminated = true;
	}

	void stopThread()
	{
		setTerminated();
		interrupt.notify_all();
		if (thread.joinable())
			thread.join();
	}

#if LOG4CXX_EVENTS_AT_EXIT
	helpers::AtExitRegistry::Raii atExitRegistryRaii;
#endif
};

#if LOG4CXX_HAS_PTHREAD_SIGMASK
	static thread_local sigset_t old_mask;
	static thread_local bool sigmask_valid;
#endif

ThreadUtility::ThreadUtility()
	: m_priv( std::make_unique<priv_data>() )
{
	// Block signals by default.
	configureFuncs( std::bind( &ThreadUtility::preThreadBlockSignals, this ),
		nullptr,
		std::bind( &ThreadUtility::postThreadUnblockSignals, this ) );
}

ThreadUtility::~ThreadUtility() {}

auto ThreadUtility::instancePtr() -> ManagerPtr
{
	auto result = APRInitializer::getOrAddUnique<Manager>
		( []() -> ObjectPtr
			{ return std::make_shared<Manager>(); }
		);
	return result;
}

ThreadUtility* ThreadUtility::instance()
{
	return &instancePtr()->value();
}

void ThreadUtility::configure( ThreadConfigurationType type )
{
	auto utility = instance();

	if ( type == ThreadConfigurationType::NoConfiguration )
	{
		utility->configureFuncs( nullptr, nullptr, nullptr );
	}
	else if ( type == ThreadConfigurationType::NameThreadOnly )
	{
		utility->configureFuncs( nullptr,
			std::bind( &ThreadUtility::threadStartedNameThread, utility,
				std::placeholders::_1,
				std::placeholders::_2,
				std::placeholders::_3 ),
			nullptr );
	}
	else if ( type == ThreadConfigurationType::BlockSignalsOnly )
	{
		utility->configureFuncs( std::bind( &ThreadUtility::preThreadBlockSignals, utility ),
			nullptr,
			std::bind( &ThreadUtility::postThreadUnblockSignals, utility ) );
	}
	else if ( type == ThreadConfigurationType::BlockSignalsAndNameThread )
	{
		utility->configureFuncs( std::bind( &ThreadUtility::preThreadBlockSignals, utility ),
			std::bind( &ThreadUtility::threadStartedNameThread, utility,
				std::placeholders::_1,
				std::placeholders::_2,
				std::placeholders::_3 ),
			std::bind( &ThreadUtility::postThreadUnblockSignals, utility ) );
	}
}

void ThreadUtility::configureFuncs( ThreadStartPre pre_start,
	ThreadStarted started,
	ThreadStartPost post_start )
{
	m_priv->start_pre = pre_start;
	m_priv->started = started;
	m_priv->start_post = post_start;
}

void ThreadUtility::preThreadBlockSignals()
{
#if LOG4CXX_HAS_PTHREAD_SIGMASK
	sigset_t set;
	sigfillset(&set);

	if ( pthread_sigmask(SIG_SETMASK, &set, &old_mask) < 0 )
	{
		LOGLOG_ERROR( LOG4CXX_STR("Unable to set thread sigmask") );
		sigmask_valid = false;
	}
	else
	{
		sigmask_valid = true;
	}

#endif /* LOG4CXX_HAS_PTHREAD_SIGMASK */
}

void ThreadUtility::threadStartedNameThread(LogString threadName,
	std::thread::id /*threadId*/,
	std::thread::native_handle_type nativeHandle)
{
#if LOG4CXX_HAS_PTHREAD_SETNAME && !(defined(_WIN32) && defined(_LIBCPP_VERSION))
	LOG4CXX_ENCODE_CHAR(sthreadName, threadName);
	if (pthread_setname_np(static_cast<pthread_t>(nativeHandle), sthreadName.c_str()) < 0) {
		LOGLOG_ERROR(LOG4CXX_STR("unable to set thread name"));
	}
#elif defined(_WIN32)
	typedef HRESULT (WINAPI *TSetThreadDescription)(HANDLE, PCWSTR);
	static struct initialiser
	{
		HMODULE hKernelBase;
		TSetThreadDescription SetThreadDescription;
		initialiser()
			: hKernelBase(GetModuleHandleA("KernelBase.dll"))
			, SetThreadDescription(nullptr)
		{
			if (hKernelBase)
				SetThreadDescription = reinterpret_cast<TSetThreadDescription>(GetProcAddress(hKernelBase, "SetThreadDescription"));
		}
	} win32Func;
	if (win32Func.SetThreadDescription)
	{
		LOG4CXX_ENCODE_WCHAR(wthreadName, threadName);
		if(FAILED(win32Func.SetThreadDescription(static_cast<HANDLE>(nativeHandle), wthreadName.c_str())))
			LOGLOG_ERROR( LOG4CXX_STR("unable to set thread name") );
	}
#endif
}

void ThreadUtility::postThreadUnblockSignals()
{
#if LOG4CXX_HAS_PTHREAD_SIGMASK

	// Only restore the signal mask if we were able to set it in the first place.
	if ( sigmask_valid )
	{
		if ( pthread_sigmask(SIG_SETMASK, &old_mask, nullptr) < 0 )
		{
			LOGLOG_ERROR( LOG4CXX_STR("Unable to set thread sigmask") );
		}
	}

#endif /* LOG4CXX_HAS_PTHREAD_SIGMASK */
}


ThreadStartPre ThreadUtility::preStartFunction()
{
	return m_priv->start_pre;
}

ThreadStarted ThreadUtility::threadStartedFunction()
{
	return m_priv->started;
}

ThreadStartPost ThreadUtility::postStartFunction()
{
	return m_priv->start_post;
}

/**
 * Add a periodic task
 */
void ThreadUtility::addPeriodicTask(const LogString& name, std::function<void()> f, const Period& delay)
{
	std::lock_guard<std::recursive_mutex> lock(m_priv->job_mutex);
	if (m_priv->maxDelay < delay)
		m_priv->maxDelay = delay;
	auto currentTime = std::chrono::system_clock::now();
	m_priv->jobs.push_back( priv_data::NamedPeriodicFunction{name, delay, currentTime + delay, f, 0, false} );
	if (!m_priv->thread.joinable())
	{
		m_priv->terminated = false;
		m_priv->thread = createThread(LOG4CXX_STR("log4cxx"), std::bind(&priv_data::doPeriodicTasks, m_priv.get()));
	}
	else
		m_priv->interrupt.notify_one();
}

/**
 * Is this already running a \c taskName periodic task?
 */
bool ThreadUtility::hasPeriodicTask(const LogString& name)
{
	std::lock_guard<std::recursive_mutex> lock(m_priv->job_mutex);
	auto pItem = std::find_if(m_priv->jobs.begin(), m_priv->jobs.end()
		, [&name](const priv_data::NamedPeriodicFunction& item)
		{ return !item.removed && name == item.name; }
		);
	return m_priv->jobs.end() != pItem;
}

/**
 * Remove all periodic tasks and stop the processing thread
 */
void ThreadUtility::removeAllPeriodicTasks()
{
	{
		std::lock_guard<std::recursive_mutex> lock(m_priv->job_mutex);
		while (!m_priv->jobs.empty())
			m_priv->jobs.pop_back();
	}
	m_priv->stopThread();
}

/**
 * Remove the \c taskName periodic task
 */
void ThreadUtility::removePeriodicTask(const LogString& name)
{
	std::lock_guard<std::recursive_mutex> lock(m_priv->job_mutex);
	auto pItem = std::find_if(m_priv->jobs.begin(), m_priv->jobs.end()
		, [&name](const priv_data::NamedPeriodicFunction& item)
		{ return !item.removed && name == item.name; }
		);
	if (m_priv->jobs.end() != pItem)
	{
		pItem->removed = true;
		m_priv->interrupt.notify_one();
	}
}

/**
 * Remove any periodic task matching \c namePrefix
 */
void ThreadUtility::removePeriodicTasksMatching(const LogString& namePrefix)
{
	while (1)
	{
		std::lock_guard<std::recursive_mutex> lock(m_priv->job_mutex);
		auto pItem = std::find_if(m_priv->jobs.begin(), m_priv->jobs.end()
			, [&namePrefix](const priv_data::NamedPeriodicFunction& item)
			{ return !item.removed && namePrefix.size() <= item.name.size() && item.name.substr(0, namePrefix.size()) == namePrefix; }
			);
		if (m_priv->jobs.end() == pItem)
			break;
		pItem->removed = true;
	}
	m_priv->interrupt.notify_one();
}

// Run ready tasks
void ThreadUtility::priv_data::doPeriodicTasks()
{
	while (!this->terminated)
	{
		auto currentTime = std::chrono::system_clock::now();
		TimePoint nextOperationTime = currentTime + this->maxDelay;
		{
			std::lock_guard<std::recursive_mutex> lock(this->job_mutex);
			for (auto& item : this->jobs)
			{
				if (this->terminated)
					return;
				if (item.removed)
					;
				else if (item.nextRun <= currentTime)
				{
					try
					{
						item.f();
						item.nextRun = std::chrono::system_clock::now() + item.delay;
						if (item.nextRun < nextOperationTime)
							nextOperationTime = item.nextRun;
						item.errorCount = 0;
					}
					catch (std::exception& ex)
					{
						LogLog::warn(item.name, ex);
						++item.errorCount;
					}
					catch (...)
					{
						LogLog::warn(item.name + LOG4CXX_STR(" threw an exception"));
						++item.errorCount;
					}
				}
				else if (item.nextRun < nextOperationTime)
					nextOperationTime = item.nextRun;
			}
		}
		// Delete removed and faulty tasks
		while (1)
		{
			std::lock_guard<std::recursive_mutex> lock(this->job_mutex);
			auto pItem = std::find_if(this->jobs.begin(), this->jobs.end()
				, [this](const NamedPeriodicFunction& item)
				{ return item.removed || this->retryCount < item.errorCount; }
				);
			if (this->jobs.end() == pItem)
				break;
			this->jobs.erase(pItem);
			if (this->jobs.empty())
				return;
		}

		std::unique_lock<std::mutex> lock(this->interrupt_mutex);
		this->interrupt.wait_until(lock, nextOperationTime);
	}
}

} //namespace helpers
} //namespace log4cxx
