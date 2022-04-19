#pragma once
#include <pthread.h>
#include "mutexLinux.h"
#include "../signal.h"

class SignalLinux : public Signal
{
public:
	SignalLinux();
	virtual ~SignalLinux();

	virtual void fire();
	virtual bool wait(u32 timeOutInMS=TIMEOUT_INFINITE, bool reset=true);

private:
	pthread_cond_t m_handle;
	MutexLinux m_mutex;
	bool m_signalled;
};
