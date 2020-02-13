#include "InstrumentationStopwatch.h"

InstrumentationStopwatch gInstrumentationStopwatch{
    {CATEGORY_TRACING, "Tracing"},
    {CATEGORY_UNWINDING, "Unwinding"},
    {CATEGORY_PROCESS_CONTEXT_SWITCH, "ProcessContextSwitch"},
    {CATEGORY_HANDLE_CALLSTACK, "HandleCallstack"},
    {CATEGORY_HANDLE_TIMER, "HandleTimer"},
    {CATEGORY_SLEEP, "OrbitSleepMs"},
};