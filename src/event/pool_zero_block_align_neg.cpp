// Negative compile contract: this TU MUST NOT compile.
//
// A zero block alignment cannot define a valid pool stride or placement-new
// address. EventPool must reject it when the class template is instantiated.
// SPDX-License-Identifier: MIT

#include <cstdint>

#include "coact/pool.hpp"

void use_zero_block_alignment()
{
    coact::EventPool<sizeof(coact::Event), 1U,
                     coact::HostSmpProfile, 0U> pool;
    (void)pool;
}
