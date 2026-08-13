// Negative compile fixture for the RT-Thread single-core PAL contract.
// The include directory selected by each CMake target supplies either an SMP
// or multi-core RT-Thread configuration.
#include "coact/pal_rtthread.hpp"

void instantiate_rtthread_pal_gate()
{
}
