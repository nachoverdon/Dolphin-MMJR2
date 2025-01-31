// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CoreTiming.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/SPSCQueue.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "VideoCommon/Fifo.h"
#include "VideoCommon/VideoBackendBase.h"

namespace CoreTiming
{
struct EventType
{
  TimedCallback callback;
  const std::string* name;
};

struct Event
{
  s64 time;
  u64 fifo_order;
  u64 userdata;
  EventType* type;
};

// Sort by time, unless the times are the same, in which case sort by the order added to the queue
static bool operator>(const Event& left, const Event& right)
{
  return std::tie(left.time, left.fifo_order) > std::tie(right.time, right.fifo_order);
}
static bool operator<(const Event& left, const Event& right)
{
  return std::tie(left.time, left.fifo_order) < std::tie(right.time, right.fifo_order);
}

static constexpr int MAX_SLICE_LENGTH = 20000;

struct CoreTimingState::Data
{
  // unordered_map stores each element separately as a linked list node so pointers to elements
  // remain stable regardless of rehashes/resizing.
  std::unordered_map<std::string, EventType> event_types;

  // STATE_TO_SAVE
  // The queue is a min-heap using std::make_heap/push_heap/pop_heap.
  // We don't use std::priority_queue because we need to be able to serialize, unserialize and
  // erase arbitrary events (RemoveEvent()) regardless of the queue order. These aren't accomodated
  // by the standard adaptor class.
  std::vector<Event> event_queue;
  u64 event_fifo_id;
  std::mutex ts_write_lock;
  Common::SPSCQueue<Event, false> ts_queue;

  float last_oc_factor;

  s64 idled_cycles;
  u32 fake_dec_start_value;
  u64 fake_dec_start_ticks;

  // Are we in a function that has been called from Advance()
  bool is_global_timer_sane;

  EventType* ev_lost = nullptr;

  size_t registered_config_callback_id;
  float config_oc_factor;
  float config_oc_inv_factor;
  bool config_sync_on_skip_idle;
};

CoreTimingState::CoreTimingState() : m_data(std::make_unique<Data>())
{
}

CoreTimingState::~CoreTimingState() = default;

static void EmptyTimedCallback(Core::System& system, u64 userdata, s64 cyclesLate)
{
}

// Changing the CPU speed in Dolphin isn't actually done by changing the physical clock rate,
// but by changing the amount of work done in a particular amount of time. This tends to be more
// compatible because it stops the games from actually knowing directly that the clock rate has
// changed, and ensures that anything based on waiting a specific number of cycles still works.
//
// Technically it might be more accurate to call this changing the IPC instead of the CPU speed,
// but the effect is largely the same.
static int DowncountToCycles(CoreTiming::Globals& g, int downcount)
{
  return static_cast<int>(downcount * g.last_OC_factor_inverted);
}

static int CyclesToDowncount(CoreTiming::CoreTimingState::Data& state, int cycles)
{
  return static_cast<int>(cycles * state.last_oc_factor);
}

EventType* RegisterEvent(const std::string& name, TimedCallback callback)
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();

  // check for existing type with same name.
  // we want event type names to remain unique so that we can use them for serialization.
  ASSERT_MSG(POWERPC, state.event_types.find(name) == state.event_types.end(),
             "CoreTiming Event \"{}\" is already registered. Events should only be registered "
             "during Init to avoid breaking save states.",
             name);

  auto info = state.event_types.emplace(name, EventType{callback, nullptr});
  EventType* event_type = &info.first->second;
  event_type->name = &info.first->first;
  return event_type;
}

void UnregisterAllEvents()
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();

  ASSERT_MSG(POWERPC, state.event_queue.empty(), "Cannot unregister events with events pending");
  state.event_types.clear();
}

void Init()
{
  auto& system = Core::System::GetInstance();
  auto& state = system.GetCoreTimingState().GetData();
  auto& g = system.GetCoreTimingGlobals();

  state.registered_config_callback_id =
      Config::AddConfigChangedCallback([]() { Core::RunAsCPUThread([]() { RefreshConfig(); }); });
  RefreshConfig();

  state.last_oc_factor = state.config_oc_factor;
  g.last_OC_factor_inverted = state.config_oc_inv_factor;
  PowerPC::ppcState.downcount = CyclesToDowncount(state, MAX_SLICE_LENGTH);
  g.slice_length = MAX_SLICE_LENGTH;
  g.global_timer = 0;
  state.idled_cycles = 0;

  // The time between CoreTiming being intialized and the first call to Advance() is considered
  // the slice boundary between slice -1 and slice 0. Dispatcher loops must call Advance() before
  // executing the first PPC cycle of each slice to prepare the slice length and downcount for
  // that slice.
  state.is_global_timer_sane = true;

  state.event_fifo_id = 0;
  state.ev_lost = RegisterEvent("_lost_event", &EmptyTimedCallback);
}

void Shutdown()
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();
  std::lock_guard lk(state.ts_write_lock);
  MoveEvents();
  ClearPendingEvents();
  UnregisterAllEvents();
  Config::RemoveConfigChangedCallback(state.registered_config_callback_id);
}

void RefreshConfig()
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();
  state.config_oc_factor =
      Config::Get(Config::MAIN_OVERCLOCK_ENABLE) ? Config::Get(Config::MAIN_OVERCLOCK) : 1.0f;
  state.config_oc_inv_factor = 1.0f / state.config_oc_factor;
  state.config_sync_on_skip_idle = Config::Get(Config::MAIN_SYNC_ON_SKIP_IDLE);
}

void DoState(PointerWrap& p)
{
  auto& system = Core::System::GetInstance();
  auto& state = system.GetCoreTimingState().GetData();
  auto& g = system.GetCoreTimingGlobals();

  std::lock_guard lk(state.ts_write_lock);
  p.Do(g.slice_length);
  p.Do(g.global_timer);
  p.Do(state.idled_cycles);
  p.Do(state.fake_dec_start_value);
  p.Do(state.fake_dec_start_ticks);
  p.Do(g.fake_TB_start_value);
  p.Do(g.fake_TB_start_ticks);
  p.Do(state.last_oc_factor);
  g.last_OC_factor_inverted = 1.0f / state.last_oc_factor;
  p.Do(state.event_fifo_id);

  p.DoMarker("CoreTimingData");

  MoveEvents();
  p.DoEachElement(state.event_queue, [&state](PointerWrap& pw, Event& ev) {
    pw.Do(ev.time);
    pw.Do(ev.fifo_order);

    // this is why we can't have (nice things) pointers as userdata
    pw.Do(ev.userdata);

    // we can't savestate ev.type directly because events might not get registered in the same
    // order (or at all) every time.
    // so, we savestate the event's type's name, and derive ev.type from that when loading.
    std::string name;
    if (!pw.IsReadMode())
      name = *ev.type->name;

    pw.Do(name);
    if (pw.IsReadMode())
    {
      auto itr = state.event_types.find(name);
      if (itr != state.event_types.end())
      {
        ev.type = &itr->second;
      }
      else
      {
        WARN_LOG_FMT(POWERPC,
                     "Lost event from savestate because its type, \"{}\", has not been registered.",
                     name);
        ev.type = state.ev_lost;
      }
    }
  });
  p.DoMarker("CoreTimingEvents");

  // When loading from a save state, we must assume the Event order is random and meaningless.
  // The exact layout of the heap in memory is implementation defined, therefore it is platform
  // and library version specific.
  if (p.IsReadMode())
    std::make_heap(state.event_queue.begin(), state.event_queue.end(), std::greater<Event>());
}

// This should only be called from the CPU thread. If you are calling
// it from any other thread, you are doing something evil
u64 GetTicks()
{
  auto& system = Core::System::GetInstance();
  auto& state = system.GetCoreTimingState().GetData();
  auto& g = system.GetCoreTimingGlobals();

  u64 ticks = static_cast<u64>(g.global_timer);
  if (!state.is_global_timer_sane)
  {
    int downcount = DowncountToCycles(g, PowerPC::ppcState.downcount);
    ticks += g.slice_length - downcount;
  }
  return ticks;
}

u64 GetIdleTicks()
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();
  return static_cast<u64>(state.idled_cycles);
}

void ClearPendingEvents()
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();
  state.event_queue.clear();
}

void ScheduleEvent(s64 cycles_into_future, EventType* event_type, u64 userdata, FromThread from)
{
  ASSERT_MSG(POWERPC, event_type, "Event type is nullptr, will crash now.");

  auto& system = Core::System::GetInstance();
  auto& state = system.GetCoreTimingState().GetData();
  auto& g = system.GetCoreTimingGlobals();

  bool from_cpu_thread;
  if (from == FromThread::ANY)
  {
    from_cpu_thread = Core::IsCPUThread();
  }
  else
  {
    from_cpu_thread = from == FromThread::CPU;
    ASSERT_MSG(POWERPC, from_cpu_thread == Core::IsCPUThread(),
               "A \"{}\" event was scheduled from the wrong thread ({})", *event_type->name,
               from_cpu_thread ? "CPU" : "non-CPU");
  }

  if (from_cpu_thread)
  {
    s64 timeout = GetTicks() + cycles_into_future;

    // If this event needs to be scheduled before the next advance(), force one early
    if (!state.is_global_timer_sane)
      ForceExceptionCheck(cycles_into_future);

    state.event_queue.emplace_back(Event{timeout, state.event_fifo_id++, userdata, event_type});
    std::push_heap(state.event_queue.begin(), state.event_queue.end(), std::greater<Event>());
  }
  else
  {
    if (Core::WantsDeterminism())
    {
      ERROR_LOG_FMT(POWERPC,
                    "Someone scheduled an off-thread \"{}\" event while netplay or "
                    "movie play/record was active.  This is likely to cause a desync.",
                    *event_type->name);
    }

    std::lock_guard lk(state.ts_write_lock);
    state.ts_queue.Push(Event{g.global_timer + cycles_into_future, 0, userdata, event_type});
  }
}

void RemoveEvent(EventType* event_type)
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();

  auto itr = std::remove_if(state.event_queue.begin(), state.event_queue.end(),
                            [&](const Event& e) { return e.type == event_type; });

  // Removing random items breaks the invariant so we have to re-establish it.
  if (itr != state.event_queue.end())
  {
    state.event_queue.erase(itr, state.event_queue.end());
    std::make_heap(state.event_queue.begin(), state.event_queue.end(), std::greater<Event>());
  }
}

void RemoveAllEvents(EventType* event_type)
{
  MoveEvents();
  RemoveEvent(event_type);
}

void ForceExceptionCheck(s64 cycles)
{
  auto& system = Core::System::GetInstance();
  auto& state = system.GetCoreTimingState().GetData();
  auto& g = system.GetCoreTimingGlobals();

  cycles = std::max<s64>(0, cycles);
  if (DowncountToCycles(g, PowerPC::ppcState.downcount) > cycles)
  {
    // downcount is always (much) smaller than MAX_INT so we can safely cast cycles to an int here.
    // Account for cycles already executed by adjusting the g.slice_length
    g.slice_length -= DowncountToCycles(g, PowerPC::ppcState.downcount) - static_cast<int>(cycles);
    PowerPC::ppcState.downcount = CyclesToDowncount(state, static_cast<int>(cycles));
  }
}

void MoveEvents()
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();
  for (Event ev; state.ts_queue.Pop(ev);)
  {
    ev.fifo_order = state.event_fifo_id++;
    state.event_queue.emplace_back(std::move(ev));
    std::push_heap(state.event_queue.begin(), state.event_queue.end(), std::greater<Event>());
  }
}

void Advance()
{
  auto& system = Core::System::GetInstance();
  auto& state = system.GetCoreTimingState().GetData();
  auto& g = system.GetCoreTimingGlobals();

  MoveEvents();

  int cyclesExecuted = g.slice_length - DowncountToCycles(g, PowerPC::ppcState.downcount);
  g.global_timer += cyclesExecuted;
  state.last_oc_factor = state.config_oc_factor;
  g.last_OC_factor_inverted = state.config_oc_inv_factor;
  g.slice_length = MAX_SLICE_LENGTH;

  state.is_global_timer_sane = true;

  while (!state.event_queue.empty() && state.event_queue.front().time <= g.global_timer)
  {
    Event evt = std::move(state.event_queue.front());
    std::pop_heap(state.event_queue.begin(), state.event_queue.end(), std::greater<Event>());
    state.event_queue.pop_back();
    evt.type->callback(system, evt.userdata, g.global_timer - evt.time);
  }

  state.is_global_timer_sane = false;

  // Still events left (scheduled in the future)
  if (!state.event_queue.empty())
  {
    g.slice_length = static_cast<int>(
        std::min<s64>(state.event_queue.front().time - g.global_timer, MAX_SLICE_LENGTH));
  }

  PowerPC::ppcState.downcount = CyclesToDowncount(state, g.slice_length);

  // Check for any external exceptions.
  // It's important to do this after processing events otherwise any exceptions will be delayed
  // until the next slice:
  //        Pokemon Box refuses to boot if the first exception from the audio DMA is received late
  PowerPC::CheckExternalExceptions();
}

void LogPendingEvents()
{
  auto& system = Core::System::GetInstance();
  auto& state = system.GetCoreTimingState().GetData();
  auto& g = system.GetCoreTimingGlobals();

  auto clone = state.event_queue;
  std::sort(clone.begin(), clone.end());
  for (const Event& ev : clone)
  {
    INFO_LOG_FMT(POWERPC, "PENDING: Now: {} Pending: {} Type: {}", g.global_timer, ev.time,
                 *ev.type->name);
  }
}

// Should only be called from the CPU thread after the PPC clock has changed
void AdjustEventQueueTimes(u32 new_ppc_clock, u32 old_ppc_clock)
{
  auto& system = Core::System::GetInstance();
  auto& state = system.GetCoreTimingState().GetData();
  auto& g = system.GetCoreTimingGlobals();

  for (Event& ev : state.event_queue)
  {
    const s64 ticks = (ev.time - g.global_timer) * new_ppc_clock / old_ppc_clock;
    ev.time = g.global_timer + ticks;
  }
}

void Idle()
{
  auto& system = Core::System::GetInstance();
  auto& state = system.GetCoreTimingState().GetData();
  auto& g = system.GetCoreTimingGlobals();

  if (state.config_sync_on_skip_idle)
  {
    // When the FIFO is processing data we must not advance because in this way
    // the VI will be desynchronized. So, We are waiting until the FIFO finish and
    // while we process only the events required by the FIFO.
    Fifo::FlushGpu();
  }

  PowerPC::UpdatePerformanceMonitor(PowerPC::ppcState.downcount, 0, 0);
  state.idled_cycles += DowncountToCycles(g, PowerPC::ppcState.downcount);
  PowerPC::ppcState.downcount = 0;
}

std::string GetScheduledEventsSummary()
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();

  std::string text = "Scheduled events\n";
  text.reserve(1000);

  auto clone = state.event_queue;
  std::sort(clone.begin(), clone.end());
  for (const Event& ev : clone)
  {
    text += fmt::format("{} : {} {:016x}\n", *ev.type->name, ev.time, ev.userdata);
  }
  return text;
}

u32 GetFakeDecStartValue()
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();
  return state.fake_dec_start_value;
}

void SetFakeDecStartValue(u32 val)
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();
  state.fake_dec_start_value = val;
}

u64 GetFakeDecStartTicks()
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();
  return state.fake_dec_start_ticks;
}

void SetFakeDecStartTicks(u64 val)
{
  auto& state = Core::System::GetInstance().GetCoreTimingState().GetData();
  state.fake_dec_start_ticks = val;
}

u64 GetFakeTBStartValue()
{
  auto& g = Core::System::GetInstance().GetCoreTimingGlobals();
  return g.fake_TB_start_value;
}

void SetFakeTBStartValue(u64 val)
{
  auto& g = Core::System::GetInstance().GetCoreTimingGlobals();
  g.fake_TB_start_value = val;
}

u64 GetFakeTBStartTicks()
{
  auto& g = Core::System::GetInstance().GetCoreTimingGlobals();
  return g.fake_TB_start_ticks;
}

void SetFakeTBStartTicks(u64 val)
{
  auto& g = Core::System::GetInstance().GetCoreTimingGlobals();
  g.fake_TB_start_ticks = val;
}

}  // namespace CoreTiming
