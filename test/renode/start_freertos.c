// FreeRTOS startup for the coact Renode fixture.
// SPDX-License-Identifier: MIT
//
// Vector table: SVC/PendSV/SysTick belong to the FreeRTOS Cortex-M3 port;
// TIM2 (external IRQ 28 -> vector 44) is the fixture's asynchronous producer.
#include <stdint.h>

extern int main(void);
extern void vPortSVCHandler(void);
extern void xPortPendSVHandler(void);
extern void xPortSysTickHandler(void);
extern void TIM2_IRQHandler(void);

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

/* The image has no newlib crt, so run the C++ static constructors ourselves.
   The harness relies on them for FreeRtosResources/FreeRtos. */
extern void (*__init_array_start[])(void) __attribute__((weak));
extern void (*__init_array_end[])(void) __attribute__((weak));

void Reset_Handler(void);
void Default_Handler(void);

__attribute__((section(".isr_vector"), used))
void (*const g_vectors[16 + 82])(void) = {
    [0] = (void (*)(void))&_estack,
    [1] = Reset_Handler,
    [2] = Default_Handler,   /* NMI */
    [3] = Default_Handler,   /* HardFault */
    [4] = Default_Handler,   /* MemManage */
    [5] = Default_Handler,   /* BusFault */
    [6] = Default_Handler,   /* UsageFault */
    [11] = vPortSVCHandler,
    [14] = xPortPendSVHandler,
    [15] = xPortSysTickHandler,
    [44] = TIM2_IRQHandler,  /* 16 + 28 */
};

void Reset_Handler(void)
{
    uint32_t* src = &_sidata;
    uint32_t* dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; ++dst) {
        *dst = 0U;
    }
    if ((__init_array_start != 0) && (__init_array_end != 0)) {
        for (void (**fn)(void) = __init_array_start;
             fn < __init_array_end; ++fn) {
            (*fn)();
        }
    }
    (void)main();
    for (;;) {
    }
}

void Default_Handler(void)
{
    for (;;) {
    }
}
