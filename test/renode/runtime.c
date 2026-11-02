/*
 * Minimal libc/libatomic surface for the freestanding coact harness.
 *
 * The target is single-core and every scenario runs to completion without
 * interrupts, so 64-bit atomics are implemented as an IRQ-masked
 * read/modify/write rather than ldrexd/strexd. The functions are only here to
 * satisfy references emitted by <atomic> and by compiler-generated memset.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint32_t irq_save(void)
{
    uint32_t primask;
    __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask) : : "memory");
    return primask;
}

static void irq_restore(uint32_t primask)
{
    __asm__ volatile("msr primask, %0" : : "r"(primask) : "memory");
}

static uint64_t load64(const volatile void* ptr)
{
    const volatile uint32_t* words = (const volatile uint32_t*)ptr;
    const uint32_t lo = words[0];
    const uint32_t hi = words[1];
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void store64(volatile void* ptr, uint64_t value)
{
    volatile uint32_t* words = (volatile uint32_t*)ptr;
    words[0] = (uint32_t)value;
    words[1] = (uint32_t)(value >> 32);
}

uint64_t __atomic_load_8(const volatile void* ptr, int memorder)
{
    (void)memorder;
    const uint32_t primask = irq_save();
    const uint64_t value = load64(ptr);
    irq_restore(primask);
    return value;
}

void __atomic_store_8(volatile void* ptr, uint64_t value, int memorder)
{
    (void)memorder;
    const uint32_t primask = irq_save();
    store64(ptr, value);
    irq_restore(primask);
}

bool __atomic_compare_exchange_8(volatile void* ptr, void* expected,
                                 uint64_t desired, bool weak,
                                 int success_memorder, int failure_memorder)
{
    (void)weak;
    (void)success_memorder;
    (void)failure_memorder;
    bool ok;
    const uint32_t primask = irq_save();
    const uint64_t current = load64(ptr);
    uint64_t* expected_word = (uint64_t*)expected;
    if (current == *expected_word) {
        store64(ptr, desired);
        ok = true;
    }
    else {
        *expected_word = current;
        ok = false;
    }
    irq_restore(primask);
    return ok;
}

uint64_t __atomic_fetch_add_8(volatile void* ptr, uint64_t value, int memorder)
{
    (void)memorder;
    const uint32_t primask = irq_save();
    const uint64_t previous = load64(ptr);
    store64(ptr, previous + value);
    irq_restore(primask);
    return previous;
}

uint64_t __atomic_exchange_8(volatile void* ptr, uint64_t value, int memorder)
{
    (void)memorder;
    const uint32_t primask = irq_save();
    const uint64_t previous = load64(ptr);
    store64(ptr, value);
    irq_restore(primask);
    return previous;
}

void* memset(void* dest, int value, size_t count)
{
    unsigned char* d = (unsigned char*)dest;
    while (count > 0U) {
        *d++ = (unsigned char)value;
        --count;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t count)
{
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (count > 0U) {
        *d++ = *s++;
        --count;
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t count)
{
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        while (count > 0U) {
            *d++ = *s++;
            --count;
        }
    }
    else {
        d += count;
        s += count;
        while (count > 0U) {
            *--d = *--s;
            --count;
        }
    }
    return dest;
}

int memcmp(const void* lhs, const void* rhs, size_t count)
{
    const unsigned char* a = (const unsigned char*)lhs;
    const unsigned char* b = (const unsigned char*)rhs;
    while (count > 0U) {
        if (*a != *b) {
            return (int)*a - (int)*b;
        }
        ++a;
        ++b;
        --count;
    }
    return 0;
}
