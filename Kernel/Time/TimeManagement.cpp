/*
 * Copyright (c) 2020, Liav A. <liavalb@hotmail.co.il>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <AK/Singleton.h>
#include <AK/StdLibExtras.h>
#include <AK/Time.h>
#include <Kernel/ACPI/Parser.h>
#include <Kernel/CommandLine.h>
#include <Kernel/Interrupts/APIC.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Time/APICTimer.h>
#include <Kernel/Time/HPET.h>
#include <Kernel/Time/HPETComparator.h>
#include <Kernel/Time/HardwareTimer.h>
#include <Kernel/Time/PIT.h>
#include <Kernel/Time/RTC.h>
#include <Kernel/Time/TimeManagement.h>
#include <Kernel/TimerQueue.h>
#include <Kernel/VM/MemoryManager.h>

//#define TIME_DEBUG

namespace Kernel {

static AK::Singleton<TimeManagement> s_the;

TimeManagement& TimeManagement::the()
{
    return *s_the;
}

bool TimeManagement::is_valid_clock_id(clockid_t clock_id)
{
    switch (clock_id) {
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        return true;
    default:
        return false;
    };
}

KResultOr<timespec> TimeManagement::current_time(clockid_t clock_id) const
{
    switch (clock_id) {
    case CLOCK_MONOTONIC:
        return monotonic_time(TimePrecision::Precise);
    case CLOCK_MONOTONIC_COARSE:
        return monotonic_time(TimePrecision::Coarse);
    case CLOCK_MONOTONIC_RAW:
        return monotonic_time_raw();
    case CLOCK_REALTIME:
        return epoch_time(TimePrecision::Precise);
    case CLOCK_REALTIME_COARSE:
        return epoch_time(TimePrecision::Coarse);
    default:
        return KResult(EINVAL);
    }
}

bool TimeManagement::is_system_timer(const HardwareTimerBase& timer) const
{
    return &timer == m_system_timer.ptr();
}

void TimeManagement::set_epoch_time(timespec ts)
{
    InterruptDisabler disabler;
    m_epoch_time = ts;
    m_remaining_epoch_time_adjustment = { 0, 0 };
}

timespec TimeManagement::monotonic_time(TimePrecision precision) const
{
    // This is the time when last updated by an interrupt.
    u64 seconds;
    u32 ticks;

    bool do_query = precision == TimePrecision::Precise && m_can_query_precise_time;

    u32 update_iteration;
    do {
        update_iteration = m_update1.load(AK::MemoryOrder::memory_order_acquire);
        seconds = m_seconds_since_boot;
        ticks = m_ticks_this_second;

        if (do_query) {
            // We may have to do this over again if the timer interrupt fires
            // while we're trying to query the information. In that case, our
            // seconds and ticks became invalid, producing an incorrect time.
            // Be sure to not modify m_seconds_since_boot and m_ticks_this_second
            // because this may only be modified by the interrupt handler
            HPET::the().update_time(seconds, ticks, true);
        }
    } while (update_iteration != m_update2.load(AK::MemoryOrder::memory_order_acquire));

    ASSERT(m_time_ticks_per_second > 0);
    ASSERT(ticks < m_time_ticks_per_second);
    u64 ns = ((u64)ticks * 1000000000ull) / m_time_ticks_per_second;
    ASSERT(ns < 1000000000ull);
    return { (long)seconds, (long)ns };
}

timespec TimeManagement::epoch_time(TimePrecision) const
{
    // TODO: Take into account precision
    timespec ts;
    u32 update_iteration;
    do {
        update_iteration = m_update1.load(AK::MemoryOrder::memory_order_acquire);
        ts = m_epoch_time;
    } while (update_iteration != m_update2.load(AK::MemoryOrder::memory_order_acquire));
    return ts;
}

u64 TimeManagement::uptime_ms() const
{
    auto mtime = monotonic_time();
    u64 ms = mtime.tv_sec * 1000ull;
    ms += mtime.tv_nsec / 1000000;
    return ms;
}

void TimeManagement::initialize(u32 cpu)
{
    if (cpu == 0) {
        ASSERT(!s_the.is_initialized());
        s_the.ensure_instance();

        // Initialize the APIC timers after the other timers as the
        // initialization needs to briefly enable interrupts, which then
        // would trigger a deadlock trying to get the s_the instance while
        // creating it.
        if (auto* apic_timer = APIC::the().initialize_timers(*s_the->m_system_timer)) {
            klog() << "Time: Using APIC timer as system timer";
            s_the->set_system_timer(*apic_timer);
        }
    } else {
        ASSERT(s_the.is_initialized());
        if (auto* apic_timer = APIC::the().get_timer()) {
            klog() << "Time: Enable APIC timer on CPU #" << cpu;
            apic_timer->enable_local_timer();
        }
    }
}

void TimeManagement::set_system_timer(HardwareTimerBase& timer)
{
    ASSERT(Processor::current().id() == 0); // This should only be called on the BSP!
    auto original_callback = m_system_timer->set_callback(nullptr);
    m_system_timer->disable();
    timer.set_callback(move(original_callback));
    m_system_timer = timer;
}

time_t TimeManagement::ticks_per_second() const
{
    return m_time_keeper_timer->ticks_per_second();
}

time_t TimeManagement::boot_time() const
{
    return RTC::boot_time();
}

TimeManagement::TimeManagement()
{
    bool probe_non_legacy_hardware_timers = !(kernel_command_line().lookup("time").value_or("modern") == "legacy");
    if (ACPI::is_enabled()) {
        if (!ACPI::Parser::the()->x86_specific_flags().cmos_rtc_not_present) {
            RTC::initialize();
            m_epoch_time.tv_sec += boot_time();
        } else {
            klog() << "ACPI: RTC CMOS Not present";
        }
    } else {
        // We just assume that we can access RTC CMOS, if ACPI isn't usable.
        RTC::initialize();
        m_epoch_time.tv_sec += boot_time();
    }
    if (probe_non_legacy_hardware_timers) {
        if (!probe_and_set_non_legacy_hardware_timers())
            if (!probe_and_set_legacy_hardware_timers())
                ASSERT_NOT_REACHED();
    } else if (!probe_and_set_legacy_hardware_timers()) {
        ASSERT_NOT_REACHED();
    }
}

timeval TimeManagement::now_as_timeval()
{
    timespec ts = s_the.ptr()->epoch_time();
    timeval tv;
    timespec_to_timeval(ts, tv);
    return tv;
}

Vector<HardwareTimerBase*> TimeManagement::scan_and_initialize_periodic_timers()
{
    bool should_enable = is_hpet_periodic_mode_allowed();
    dbg() << "Time: Scanning for periodic timers";
    Vector<HardwareTimerBase*> timers;
    for (auto& hardware_timer : m_hardware_timers) {
        if (hardware_timer.is_periodic_capable()) {
            timers.append(&hardware_timer);
            if (should_enable)
                hardware_timer.set_periodic();
        }
    }
    return timers;
}

Vector<HardwareTimerBase*> TimeManagement::scan_for_non_periodic_timers()
{
    dbg() << "Time: Scanning for non-periodic timers";
    Vector<HardwareTimerBase*> timers;
    for (auto& hardware_timer : m_hardware_timers) {
        if (!hardware_timer.is_periodic_capable())
            timers.append(&hardware_timer);
    }
    return timers;
}

bool TimeManagement::is_hpet_periodic_mode_allowed()
{
    auto hpet_mode = kernel_command_line().lookup("hpet").value_or("periodic");
    if (hpet_mode == "periodic")
        return true;
    if (hpet_mode == "nonperiodic")
        return false;
    ASSERT_NOT_REACHED();
}

bool TimeManagement::probe_and_set_non_legacy_hardware_timers()
{
    if (!ACPI::is_enabled())
        return false;
    if (!HPET::test_and_initialize())
        return false;
    if (!HPET::the().comparators().size()) {
        dbg() << "HPET initialization aborted.";
        return false;
    }
    dbg() << "HPET: Setting appropriate functions to timers.";

    for (auto& hpet_comparator : HPET::the().comparators())
        m_hardware_timers.append(hpet_comparator);

    auto periodic_timers = scan_and_initialize_periodic_timers();
    auto non_periodic_timers = scan_for_non_periodic_timers();

    if (is_hpet_periodic_mode_allowed())
        ASSERT(!periodic_timers.is_empty());

    ASSERT(periodic_timers.size() + non_periodic_timers.size() > 0);

    if (periodic_timers.size() > 0)
        m_system_timer = periodic_timers[0];
    else
        m_system_timer = non_periodic_timers[0];

    m_system_timer->set_callback([this](const RegisterState& regs) {
        // Update the time. We don't really care too much about the
        // frequency of the interrupt because we'll query the main
        // counter to get an accurate time.
        if (Processor::current().id() == 0) {
            // TODO: Have the other CPUs call system_timer_tick directly
            increment_time_since_boot_hpet();
        }

        system_timer_tick(regs);
    });

    // Use the HPET main counter frequency for time purposes. This is likely
    // a much higher frequency than the interrupt itself and allows us to
    // keep a more accurate time
    m_can_query_precise_time = true;
    m_time_ticks_per_second = HPET::the().frequency();

    m_system_timer->try_to_set_frequency(m_system_timer->calculate_nearest_possible_frequency(OPTIMAL_TICKS_PER_SECOND_RATE));

    // We don't need an interrupt for time keeping purposes because we
    // can query the timer.
    m_time_keeper_timer = m_system_timer;
    return true;
}

bool TimeManagement::probe_and_set_legacy_hardware_timers()
{
    if (ACPI::is_enabled()) {
        if (ACPI::Parser::the()->x86_specific_flags().cmos_rtc_not_present) {
            dbg() << "ACPI: CMOS RTC Not Present";
            return false;
        } else {
            dbg() << "ACPI: CMOS RTC Present";
        }
    }

    m_hardware_timers.append(PIT::initialize(TimeManagement::update_time));
    m_hardware_timers.append(RealTimeClock::create(TimeManagement::system_timer_tick));
    m_time_keeper_timer = m_hardware_timers[0];
    m_system_timer = m_hardware_timers[1];

    // The timer is only as accurate as the interrupts...
    m_time_ticks_per_second = m_time_keeper_timer->ticks_per_second();
    return true;
}

void TimeManagement::update_time(const RegisterState&)
{
    TimeManagement::the().increment_time_since_boot();
}

void TimeManagement::increment_time_since_boot_hpet()
{
    ASSERT(!m_time_keeper_timer.is_null());
    ASSERT(m_time_keeper_timer->timer_type() == HardwareTimerType::HighPrecisionEventTimer);

    // NOTE: m_seconds_since_boot and m_ticks_this_second are only ever
    // updated here! So we can safely read that information, query the clock,
    // and when we're all done we can update the information. This reduces
    // contention when other processors attempt to read the clock.
    auto seconds_since_boot = m_seconds_since_boot;
    auto ticks_this_second = m_ticks_this_second;
    auto delta_ns = HPET::the().update_time(seconds_since_boot, ticks_this_second, false);

    // Now that we have a precise time, go update it as quickly as we can
    u32 update_iteration = m_update1.fetch_add(1, AK::MemoryOrder::memory_order_acquire);
    m_seconds_since_boot = seconds_since_boot;
    m_ticks_this_second = ticks_this_second;
    // TODO: Apply m_remaining_epoch_time_adjustment
    timespec_add(m_epoch_time, { (time_t)(delta_ns / 1000000000), (long)(delta_ns % 1000000000) }, m_epoch_time);
    m_update2.store(update_iteration + 1, AK::MemoryOrder::memory_order_release);
}

void TimeManagement::increment_time_since_boot()
{
    ASSERT(!m_time_keeper_timer.is_null());

    // Compute time adjustment for adjtime. Let the clock run up to 1% fast or slow.
    // That way, adjtime can adjust up to 36 seconds per hour, without time getting very jumpy.
    // Once we have a smarter NTP service that also adjusts the frequency instead of just slewing time, maybe we can lower this.
    constexpr long NanosPerTick = 1'000'000; // FIXME: Don't assume that one tick is 1 ms.
    constexpr time_t MaxSlewNanos = NanosPerTick / 100;
    static_assert(MaxSlewNanos < NanosPerTick);

    u32 update_iteration = m_update1.fetch_add(1, AK::MemoryOrder::memory_order_acquire);

    // Clamp twice, to make sure intermediate fits into a long.
    long slew_nanos = clamp(clamp(m_remaining_epoch_time_adjustment.tv_sec, (time_t)-1, (time_t)1) * 1'000'000'000 + m_remaining_epoch_time_adjustment.tv_nsec, -MaxSlewNanos, MaxSlewNanos);
    timespec slew_nanos_ts;
    timespec_sub({ 0, slew_nanos }, { 0, 0 }, slew_nanos_ts); // Normalize tv_nsec to be positive.
    timespec_sub(m_remaining_epoch_time_adjustment, slew_nanos_ts, m_remaining_epoch_time_adjustment);

    timespec epoch_tick = { .tv_sec = 0, .tv_nsec = NanosPerTick };
    epoch_tick.tv_nsec += slew_nanos; // No need for timespec_add(), guaranteed to be in range.
    timespec_add(m_epoch_time, epoch_tick, m_epoch_time);

    if (++m_ticks_this_second >= m_time_keeper_timer->ticks_per_second()) {
        // FIXME: Synchronize with other clock somehow to prevent drifting apart.
        ++m_seconds_since_boot;
        m_ticks_this_second = 0;
    }
    m_update2.store(update_iteration + 1, AK::MemoryOrder::memory_order_release);
}

void TimeManagement::system_timer_tick(const RegisterState& regs)
{
    if (Processor::current().in_irq() <= 1) {
        // Don't expire timers while handling IRQs
        TimerQueue::the().fire();
    }
    Scheduler::timer_tick(regs);
}

}
