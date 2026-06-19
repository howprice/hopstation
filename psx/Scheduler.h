// Scheduler
//
// #TODO: Move this out of PSX folder - it is not PSX specific.

#pragma once

#include "core/Types.h"

inline bool s_logScheduler = false;

class Scheduler
{
public:

	static const unsigned int kLog2MaxEvents = 4;
	static constexpr unsigned int kMaxEvents = 1 << kLog2MaxEvents;

	typedef void (*Callback)(void* userdata);

	Scheduler();

	// Returned EventID can be used to cancel the event.
	// param name must be persistent for the duration of the event
	u32 Schedule(Callback callback, void* userdata, unsigned int cyclesUntilEvent, const char* name);

	// Cancel event
	// If event has already expired, then this will be a no-op.
	void Cancel(u32 id);

	void Tick(unsigned int cycles);

	void Reset();

	const char* GetEventDebugName(u32 id) const;

private:

	struct Event
	{
		const char* name = nullptr; // (first member of struct for convenience in debugger in lieu of .natvis)

		// Doubly linked list for efficient removal
		Event* pPrev = nullptr;
		Event* pNext = nullptr;

		Callback callback = nullptr;
		void* userdata = nullptr;
		u64 expireTime = 0;

		// Event generation protects against clients cancelling recycled events.
		// The client may hold an event ID to an event that has been cancelled internally
		// when it expired. That event may have been recycled, and we cannot risk that event
		// index alone to be used as an identifier else it could result in an unrelated event
		// being cancelled. So we use monotonically increasing generation index too.
		unsigned int generation = 0;

		bool active = false;
	};

	u64 m_time = 0; // the last time at which the scheduler was updated

	Event m_events[kMaxEvents];
	Event* m_pFreeList = nullptr;
	Event* m_pLiveList = nullptr;
};
