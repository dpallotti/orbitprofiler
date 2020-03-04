#include "InstrumentationStopwatch.h"

namespace LinuxTracing {

InstrumentationStopwatch gInstrumentationStopwatch{
    {CATEGORY_TRACING, "Tracing"},
    {CATEGORY_UNWINDING, "Unwinding"},
    {CATEGORY_LISTENER, "listener->On"},
    {CATEGORY_SLEEP, "usleep"},
};

}
