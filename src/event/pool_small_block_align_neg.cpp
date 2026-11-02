// Negative compile contract: this TU MUST NOT compile.
//
// A block alignment smaller than alignof(Event) cannot safely begin an Event
// lifetime with placement new. EventPool must reject it at instantiation.
// SPDX-License-Identifier: MIT

#include "coact/pool.hpp"

#include <cstdint>

void use_undersized_block_alignment() {
  coact::EventPool<sizeof(coact::Event), 1U, coact::HostSmpProfile, 1U> pool;
  (void)pool;
}
