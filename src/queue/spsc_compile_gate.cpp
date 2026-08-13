// Negative compile fixture for the SpscRing capacity constraint.
// Compiled by the test_spsc_gate CTest with WILL_FAIL TRUE: the class
// static_assert must reject an invalid capacity at instantiation time.
// COACT_GATE_CAPACITY_ONE selects the capacity-1 (< 2) case, otherwise a
// non-power-of-two capacity is instantiated.
// SPDX-License-Identifier: MIT

#include <cstdint>

#include "coact/spsc_ring.hpp"

void instantiate_spsc_invalid_gate()
{
#ifdef COACT_GATE_CAPACITY_ONE
    coact::SpscRing<int, 1> q;   // rejected: capacity < 2
#else
    coact::SpscRing<int, 6> q;   // rejected: non-power-of-two
#endif
    (void)q;
}
