// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Timer.h"

#include <chrono>
#include <string>

#ifdef _WIN32
#include <cwchar>

#include <Windows.h>
#include <mmsystem.h>
#include <sys/timeb.h>
#else
#include <sys/time.h>
#endif

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"

namespace Common
{
template <typename Clock, typename Duration>
static typename Clock::rep time_now()
{
  return std::chrono::time_point_cast<Duration>(Clock::now()).time_since_epoch().count();
}

template <typename Duration>
static auto steady_time_now()
{
  return time_now<std::chrono::steady_clock, Duration>();
}

u64 Timer::NowUs()
{
  return steady_time_now<std::chrono::microseconds>();
}

u64 Timer::NowMs()
{
  return steady_time_now<std::chrono::milliseconds>();
}

void Timer::Start()
{
  m_start_ms = NowMs();
  m_end_ms = 0;
  m_running = true;
}

void Timer::StartWithOffset(u64 offset)
{
  Start();
  if (m_start_ms > offset)
    m_start_ms -= offset;
}

void Timer::Stop()
{
  m_end_ms = NowMs();
  m_running = false;
}

u64 Timer::ElapsedMs() const
{
  // If we have not started yet, return zero
  if (m_start_ms == 0)
    return 0;

  if (m_running)
  {
    u64 now = NowMs();
    if (m_start_ms >= now)
      return 0;
    return now - m_start_ms;
  }
  else
  {
    if (m_start_ms >= m_end_ms)
      return 0;
    return m_end_ms - m_start_ms;
  }
}

u64 Timer::GetLocalTimeSinceJan1970()
{
  // TODO Would really, really like to use std::chrono here, but Windows did not support
  // std::chrono::current_zone() until 19H1, and other compilers don't even provide support for
  // timezone-related parts of chrono. Someday!
  // see https://bugs.dolphin-emu.org/issues/13007#note-4
  time_t sysTime, tzDiff, tzDST;
  time(&sysTime);
  tm* gmTime = localtime(&sysTime);

  // Account for DST where needed
  if (gmTime->tm_isdst == 1)
    tzDST = 3600;
  else
    tzDST = 0;

  // Lazy way to get local time in sec
  gmTime = gmtime(&sysTime);
  tzDiff = sysTime - mktime(gmTime);

  return static_cast<u64>(sysTime + tzDiff + tzDST);
}

double Timer::GetSystemTimeAsDouble()
{
  // FYI: std::chrono::system_clock epoch is not required to be 1970 until c++20.
  // We will however assume time_t IS unix time.
  using Clock = std::chrono::system_clock;

  // TODO: Use this on switch to c++20:
  // const auto since_epoch = Clock::now().time_since_epoch();
  const auto unix_epoch = Clock::from_time_t({});
  const auto since_epoch = Clock::now() - unix_epoch;

  const auto since_double_time_epoch = since_epoch - std::chrono::seconds(DOUBLE_TIME_OFFSET);
  return std::chrono::duration_cast<std::chrono::duration<double>>(since_double_time_epoch).count();
}

std::string Timer::SystemTimeAsDoubleToString(double time)
{
  // revert adjustments from GetSystemTimeAsDouble() to get a normal Unix timestamp again
  time_t seconds = (time_t)time + DOUBLE_TIME_OFFSET;
  tm* localTime = localtime(&seconds);

#ifdef _WIN32
  wchar_t tmp[32] = {};
  wcsftime(tmp, std::size(tmp), L"%x %X", localTime);
  return WStringToUTF8(tmp);
#else
  char tmp[32] = {};
  strftime(tmp, sizeof(tmp), "%x %X", localTime);
  return tmp;
#endif
}

void Timer::IncreaseResolution()
{
#ifdef _WIN32
  timeBeginPeriod(1);
#endif
}

void Timer::RestoreResolution()
{
#ifdef _WIN32
  timeEndPeriod(1);
#endif
}

}  // Namespace Common
