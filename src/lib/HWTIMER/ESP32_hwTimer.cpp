#if defined(PLATFORM_ESP32)
#include "hwTimer.h"
#include "logging.h"

void (*hwTimer::callbackTick)() = nullptr;
void (*hwTimer::callbackTock)() = nullptr;

volatile bool hwTimer::running = false;
volatile bool hwTimer::isTick = false;

volatile uint32_t hwTimer::HWtimerInterval = TimerIntervalUSDefault;
volatile int32_t hwTimer::PhaseShift = 0;
volatile int32_t hwTimer::FreqOffset = 0;

// Internal implementation specific variables
static hw_timer_t *timer = NULL;
static portMUX_TYPE isrMutex = portMUX_INITIALIZER_UNLOCKED;

#if defined(TARGET_RX)
#define HWTIMER_TICKS_PER_US 5
#else
#define HWTIMER_TICKS_PER_US 1
#endif

// twatch (WE7): Arduino-ESP32 3.x replaced the timer API (gptimer underneath): timerBegin(frequency), no edge flag,
// timerAlarm(value, autoreload, count) both sets and enables the alarm, timerStop / timerStart instead of
// timerAlarmDisable / timerAlarmEnable. Same period semantics: the counter reloads to 0 at each alarm.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define HWTIMER_CORE3 1
static inline void timerAlarmWrite(hw_timer_t *t, uint64_t value, bool autoreload) { timerAlarm(t, value, autoreload, 0); }
#endif

void ICACHE_RAM_ATTR hwTimer::init(void (*callbackTick)(), void (*callbackTock)())
{

    if (!timer)
    {
        hwTimer::callbackTick = callbackTick;
        hwTimer::callbackTock = callbackTock;
#if defined(HWTIMER_CORE3)
        timer = timerBegin(1000000 * HWTIMER_TICKS_PER_US);
        timerStop(timer); // timerBegin() starts it; resume() starts it with the alarm set
        timerAttachInterrupt(timer, hwTimer::callback);
#else
        timer = timerBegin(0, (APB_CLK_FREQ / 1000000 / HWTIMER_TICKS_PER_US), true);
        timerAttachInterrupt(timer, hwTimer::callback, true);
#endif
        DBGLN("hwTimer Init");
    }
}

void ICACHE_RAM_ATTR hwTimer::stop()
{
    if (timer && running)
    {
        running = false;
#if defined(HWTIMER_CORE3)
        timerStop(timer);
#else
        timerAlarmDisable(timer);
#endif
        DBGLN("hwTimer stop");
    }
}

void ICACHE_RAM_ATTR hwTimer::resume()
{
    if (timer && !running)
    {
        // The timer must be restarted so that the new timerAlarmWrite() period is set.
        timerRestart(timer);
#if defined(TARGET_TX)
        timerAlarmWrite(timer, HWtimerInterval, true);
#else
        // We want the timer to fire tock() ASAP after enabling
        // tock() should always be the first event to maintain consistency
        isTick = false;
        // When using EDGE triggered timer, enabling the timer causes an edge so the interrupt
        // is fired immediately
        // Unlike the 8266 timer, the ESP32 timer can be started without delay.
        // It does not interrupt the currently running IsrCallback(), but triggers immediately once it has completed.
        timerAlarmWrite(timer, 0 * HWTIMER_TICKS_PER_US, true);
#endif
        running = true;
#if defined(HWTIMER_CORE3)
        timerStart(timer);
#else
        timerAlarmEnable(timer);
#endif
        DBGLN("hwTimer resume");
    }
}

void ICACHE_RAM_ATTR hwTimer::updateInterval(uint32_t time)
{
    // timer should not be running when updateInterval() is called
    HWtimerInterval = time * HWTIMER_TICKS_PER_US;
    if (timer)
    {
        DBGLN("hwTimer interval: %d", time);
        timerAlarmWrite(timer, HWtimerInterval, true);
    }
}

void ICACHE_RAM_ATTR hwTimer::phaseShift(int32_t newPhaseShift)
{
    int32_t minVal = -(HWtimerInterval >> 2);
    int32_t maxVal = (HWtimerInterval >> 2);

    // phase shift is in microseconds
    PhaseShift = constrain(newPhaseShift, minVal, maxVal) * HWTIMER_TICKS_PER_US;
}

void ICACHE_RAM_ATTR hwTimer::callback(void)
{
    if (running)
    {
        portENTER_CRITICAL_ISR(&isrMutex);
#if defined(TARGET_TX)
        callbackTock();
#else
        uint32_t NextInterval = (HWtimerInterval >> 1) + FreqOffset;
        if (hwTimer::isTick)
        {
            timerAlarmWrite(timer, NextInterval, true);
            hwTimer::callbackTick();
        }
        else
        {
            NextInterval += PhaseShift;
            timerAlarmWrite(timer, NextInterval, true);
            PhaseShift = 0;
            hwTimer::callbackTock();
        }
        hwTimer::isTick = !hwTimer::isTick;
#endif
        portEXIT_CRITICAL_ISR(&isrMutex);
    }
}

#endif
