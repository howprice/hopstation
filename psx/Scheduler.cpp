#include "Scheduler.h"

#include "core/hp_assert.h"
#include "core/Log.h"

struct EventID
{
	explicit EventID(u32 val_) : val(val_) {}
	EventID(unsigned int index, unsigned int generation);
	unsigned int GetIndex() const;
	unsigned int GetGeneration() const;
	u32 val = 0;
};

EventID::EventID(unsigned int index, unsigned int generation)
{
	HP_ASSERT(index < Scheduler::kMaxEvents);
	HP_ASSERT(generation > 0);
	val = (generation << Scheduler::kLog2MaxEvents) | index;
}

unsigned int EventID::GetIndex() const
{
	constexpr u32 kMask = Scheduler::kMaxEvents - 1;
	return val & kMask;
}

unsigned int EventID::GetGeneration() const
{
	return val >> Scheduler::kLog2MaxEvents;
}

//-------------------------------------------------------------------------------

Scheduler::Scheduler()
{
	Reset();
}

void Scheduler::Reset()
{
	m_time = 0;

	// Reset all events and add to the free list
	m_pFreeList = &m_events[0];
	Event* pPrev = nullptr;
	for (unsigned int eventIndex = 0; eventIndex < kMaxEvents; eventIndex++)
	{
		Event& event = m_events[eventIndex];
		event = {};
		event.pPrev = pPrev;
		pPrev = &event;
		if (eventIndex + 1 < kMaxEvents)
			event.pNext = &m_events[eventIndex + 1];
	}

	m_pLiveList = nullptr;
}

u32 Scheduler::Schedule(Callback callback, void* userdata, unsigned int cyclesUntilEvent, const char* name)
{
	HP_ASSERT(m_pFreeList, "No free events");

	// Remove event from head of free list
	Event& event = *m_pFreeList;
	HP_ASSERT(event.pPrev == nullptr);
	m_pFreeList = event.pNext;
	if (m_pFreeList)
		m_pFreeList->pPrev = nullptr;
	event.pNext = nullptr;

	event.expireTime = m_time + cyclesUntilEvent;
	event.callback = callback;
	event.userdata = userdata;
	event.active = true;
	event.generation++;
	event.name = name;

	// Add to live list, which is sorted by expiration time
	Event* pAfter = nullptr;
	Event* p = m_pLiveList;
	while (p)
	{
		if (p->expireTime > event.expireTime)
			break;

		pAfter = p;
		p = p->pNext;
	}

	if (pAfter)
	{
		// Insert event after pAfter
		event.pNext = pAfter->pNext;
		if (event.pNext)
			event.pNext->pPrev = &event;
		pAfter->pNext = &event;
		event.pPrev = pAfter;
	}
	else if (m_pLiveList)
	{
		// Append to start of live list
		event.pNext = m_pLiveList;
		m_pLiveList->pPrev = &event;
		m_pLiveList = &event;
	}
	else // !m_pLiveList
		m_pLiveList = &event;

	if (s_logScheduler)
	{
		LOG_INFO("[Scheduler] %016x Scheduled \"%s\" to expire at %016x (%xh cycles)\n", m_time, name, event.expireTime, cyclesUntilEvent);
		if (m_pLiveList->pNext)
		{
			const Event* pEvent = m_pLiveList;
			while (pEvent)
			{
				LOG_INFO("[Scheduler]     %016x: \"%s\"\n", pEvent->expireTime, pEvent->name);
				pEvent = pEvent->pNext;
			}
		}
	}
	unsigned int eventIndex = (unsigned int)(&event - m_events);
	u32 id = EventID(eventIndex, event.generation).val;

	return id;
}

void Scheduler::Cancel(u32 id)
{
	EventID eventID(id);
	unsigned int index = eventID.GetIndex();
	Event* pEvent = m_events + index;
	if (!pEvent->active)
		return;

	unsigned int generation = eventID.GetGeneration();
	HP_DEBUG_ASSERT(generation <= pEvent->generation); // This assert may not be valid for very long running processes in which generation wraps to the number of bits allocated in the 32-bit event ID.

	if (pEvent->generation == generation)
	{
		if (s_logScheduler)
			LOG_INFO("[Scheduler] %016x Cancelled \"%s\" due at %016x\n", m_time, pEvent->name, pEvent->expireTime);

		pEvent->active = false;

		// Not really required to reset these fields, but reduces noise in the debugger.
		pEvent->name = nullptr;
		pEvent->callback = nullptr;
		pEvent->userdata = nullptr;
		pEvent->expireTime = 0;

		// Remove event from live list
		if (pEvent->pPrev)
			pEvent->pPrev->pNext = pEvent->pNext;
		else
			m_pLiveList = pEvent->pNext;
		if (pEvent->pNext)
			pEvent->pNext->pPrev = pEvent->pPrev;
		pEvent->pPrev = nullptr;
		pEvent->pNext = nullptr;

		// Add to free list
		if (m_pFreeList)
		{
			pEvent->pNext = m_pFreeList;
			m_pFreeList->pPrev = pEvent;
		}
		m_pFreeList = pEvent;
	}
}

void Scheduler::Tick(unsigned int cycles)
{
	const u64 targetTime = m_time + cycles;

	// The live list is sorted in order of increasing expiration time
	Event* pEvent = m_pLiveList;
	while (m_time < targetTime)
	{
		// Limit step by next timer expiration time.
		u64 nextTime = targetTime;

		if (pEvent && pEvent->expireTime < nextTime)
			nextTime = pEvent->expireTime;

		// An event callback may schedule another event, so m_time needs to be updated now, because it is read in Schedule().
		u64 deltaTime = nextTime - m_time;
		m_time += deltaTime;

		while (pEvent)
		{
			if (pEvent->expireTime > m_time)
				break;

			if (s_logScheduler)
				LOG_INFO("[Scheduler] %016x Calling and cancelling \"%s\"\n", m_time, pEvent->name);

			// A callback may schedule another callback, so remove this one first to keep things tidy and avoid unnecessary overhead.
			Callback callback = pEvent->callback;
			void* userdata = pEvent->userdata;

			// Remove event from live list (it is at the head)
			HP_DEBUG_ASSERT(pEvent->pPrev == nullptr);
			m_pLiveList = pEvent->pNext;
			if (m_pLiveList)
				m_pLiveList->pPrev = nullptr;
			pEvent->pPrev = nullptr;
			pEvent->pNext = nullptr;

			pEvent->active = false;

			// Not really required to reset these fields, but might help to debug
			pEvent->name = nullptr;
			pEvent->callback = nullptr;
			pEvent->userdata = nullptr;
			pEvent->expireTime = 0;

			// Add to free list
			if (m_pFreeList)
			{
				pEvent->pNext = m_pFreeList;
				m_pFreeList->pPrev = pEvent;
			}
			m_pFreeList = pEvent;

			// Now call the event callback
			callback(userdata);

			// The live event list is sorted by expiration time
			pEvent = m_pLiveList;
		}
	}
}

const char* Scheduler::GetEventDebugName(u32 id) const
{
	EventID eventID(id);
	unsigned int index = eventID.GetIndex();
	const Event* pEvent = m_events + index;
	if (!pEvent->active)
		return nullptr;

	unsigned int generation = eventID.GetGeneration();
	HP_DEBUG_ASSERT(generation <= pEvent->generation);

	if (pEvent->generation != generation)
		return nullptr;

	return pEvent->name;
}
