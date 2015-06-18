#ifndef _PTHREAD_H_
#define _PTHREAD_H_

#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <vector>
#include "debug.h"

#if defined DEBUG
	#define tDEBUG(level, x) DEBUG(level, name() << "[" << id() << "]" << x)
#else
#define tDEBUG(level, x)
#endif

class Pthread;
class Pconditional;
class Plockable
{
public:
	friend class Pthread;
	friend class Pconditional;
	Plockable() : _lock(PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP) {}
	inline void lock()
	{
		pthread_mutex_lock(&_lock);
	}
	inline void unlock()
	{
		pthread_mutex_unlock(&_lock);
	}
	inline void trylock()
	{
		pthread_mutex_trylock(&_lock);
	}
private:
	pthread_mutex_t _lock;
};

class Pconditional : private Plockable
{
public:
	Pconditional() : Plockable(), _cond(PTHREAD_COND_INITIALIZER) {}
	inline void wait(int64_t timeout_ns=-1)
	{
		// acquire lock for that condition variable
		lock();
		// either wait infinitely ...
		if (timeout_ns <0) pthread_cond_wait(&_cond, &_lock);
		else
		{
			timespec ts; clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
			ts.tv_nsec += timeout_ns % 1000000000;
			if (ts.tv_nsec > 1000000000)
			{
				ts.tv_sec += ts.tv_nsec / 1000000000;
				ts.tv_nsec %= 1000000000;
			}
			if (timeout_ns > 1000000000) ts.tv_sec += (timeout_ns/1000000000);
			// ... or a specified time
			pthread_cond_timedwait(&_cond, &_lock, &ts);
		}
		unlock();
	}
	inline void notify()
	{
		// notify ONE thread waiting for that condition
		lock();
		pthread_cond_signal(&_cond);
		unlock();
	}
	inline void notifyAll()
	{
		// notify ALL threads waiting for that condition
		lock();
		pthread_cond_broadcast(&_cond);
		unlock();
	}
private:
	pthread_cond_t _cond;
};

// pthread wrapper function that calls Pthread::loop()
void* threadfn(void*);
class Pthread
{
	friend void* threadfn(void*);
public:
	enum pt_State
	{
		pts_STOP = 0,		// thread fn not running
		pts_RUN = (1<<0),	// thread fn running but not doing loop()
		pts_TERMINATE = (1<<1), // thread fn running and about to terminate
	};
	Pthread(const char* name, bool dostart=true);

	virtual void loop() {} // = 0, for valgrind

	inline void start()
	{
		// check if we are not running
		if (pts_STOP != _state ) return;

		// if this is restart, join old thread first
		if (thread_id) join();

		// create a new thread with this as argument and the threadfn helper as
		// body function
		pthread_create(&thread_id, NULL, threadfn, this);

		// set state to RUN
		_state = pts_RUN;
	}

	int join()
	{
		int res;
		pthread_join(thread_id, (void**) &res);
		return res;
	}
	inline void lock(Plockable &_lock = _global_lock)
	{
		_lock.lock();
	}
	inline void unlock(Plockable &_lock = _global_lock)
	{
		_lock.unlock();
	}
	static inline void sleepUntil(Pconditional &condition=_global_condition, int64_t timeout_ns=-1)
	{
		condition.wait(timeout_ns);
	}
	static inline void wake(Pconditional &condition=_global_condition)
	{
		condition.notify();
	}
	static inline void wakeAll(Pconditional &condition=_global_condition)
	{
		condition.notifyAll();
	}
	int id() const
	{
		return _id;
	}
	void cancel(bool force=false)
	{
		// gracious, soft cancel
		_state |= pts_TERMINATE;

		if (force)
		{
			// the hard way
			usleep (1000);
			if (_state != pts_STOP)
				pthread_cancel(thread_id);
		}
	}
	const char* name() const
	{
		return _name;
	}

	uint32_t getThreadState() const { return _state; }

private:
	static Pconditional _global_condition;
	static Plockable _global_lock; // one global mutex to sync all threads
	uint32_t _state;	  // thread state
	static int num_id;    // increased with every created thread
	int _id;              // id of this thread
	pthread_t thread_id;  // thread id by pthread_create
	const char* _name;    // name of the thread
};

struct PthreadRegister : private std::vector<Pthread*>
{
	friend class Pthread;
	void cancel(bool force = false)
	{
		for (auto &t: *this)
			t->cancel(force);
	}
};
extern PthreadRegister allThreads;

#endif // _PTHREAD_H_
