#include "signalLinux.h"

SignalLinux::SignalLinux()
{
	pthread_cond_init(&m_handle, NULL);
	m_signalled = false;
}

SignalLinux::~SignalLinux()
{
	pthread_cond_destroy(&m_handle);
}

void SignalLinux::fire()
{
	m_mutex.lock();
	m_signalled = true;
	m_mutex.unlock();
	pthread_cond_signal(&m_handle);
}

bool SignalLinux::wait(u32 timeOutInMS, bool reset)
{
	timespec timeout = {
		.tv_sec = timeOutInMS / 1000,
		.tv_nsec = (timeOutInMS % 1000) * 1000000,
	};

	m_mutex.lock();

	int res = 0;
	while (!m_signalled && res == 0)
	{
		res = pthread_cond_timedwait(&m_handle, &m_mutex.m_handle, &timeout);
	}

	//reset the event so it can be used again but only if the event was signaled.
	if (res == 0 && reset)
	{
		m_signalled = false;
	}

	m_mutex.unlock();

	return res == 0;
}

//factory
Signal* Signal::create()
{
	return new SignalLinux();
}

