// Standalone ARM analysis probe for the stage-1 SPSC hard checkpoint A.
// Compiled OUTSIDE CMake with arm-none-eabi-g++ to measure the target codegen:
//   - hot-path instruction counts (objdump -d)
//   - .text/.rodata sizes (arm-none-eabi-size / nm)
//   - libatomic undefined symbols (arm-none-eabi-nm -u)
//
// The irq-mask ring hooks model the RT-Thread Cortex-M PRIMASK-based
// rt_hw_interrupt_disable/enable reached through the coact CriticalSection
// function-pointer hooks (the real production path, hence the call overhead).
//
// Compile:
//   arm-none-eabi-g++ -mcpu=cortex-m4 -mthumb -std=c++17 -fno-exceptions \
//     -fno-rtti -fno-threadsafe-statics -O2 -c arm_ring_probe.cpp
// SPDX-License-Identifier: MIT

#include <cstdint>

#include "coact/queue.hpp"
#include "coact/spsc_ring.hpp"

namespace {

uintptr_t probe_irq_save(void*)
{
    uintptr_t primask = 0U;
    __asm__ volatile("mrs %0, primask" : "=r"(primask));
    __asm__ volatile("cpsid i");
    return primask;
}

void probe_irq_restore(void*, uintptr_t level)
{
    __asm__ volatile("msr primask, %0" : : "r"(level));
}

coact::CriticalSection g_cs;

}  // namespace

using Spsc = coact::SpscRing<uint16_t, 8>;
using Ring = coact::SingleCoreCriticalRing<uint16_t, 8>;

extern "C" {

void spsc_probe_init(Spsc* q) { (void)q; }
void ring_probe_init(Ring* q) { (void)q; }

bool spsc_push(Spsc* q, uint16_t v)
{
    return q->try_push(static_cast<uint16_t>(v));
}
bool spsc_pop(Spsc* q, uint16_t* out)
{
    return q->try_pop(*out);
}
uint16_t spsc_size(const Spsc* q)
{
    return q->size();
}

bool ring_push(Ring* q, uint16_t v)
{
    return q->try_push(static_cast<uint16_t>(v));
}
bool ring_pop(Ring* q, uint16_t* out)
{
    return q->try_pop(*out);
}
uint16_t ring_size(const Ring* q)
{
    return q->size();
}

void probe_cs_init()
{
    g_cs.ctx = nullptr;
    g_cs.save = &probe_irq_save;
    g_cs.restore = &probe_irq_restore;
}

}  // extern "C"
