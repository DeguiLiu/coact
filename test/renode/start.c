/*
 * Minimal Cortex-M4 startup for the coact Renode harness.
 *
 * No libc: copy .data from flash, zero .bss, call main. The fault handlers are
 * supplied by the linked test program (main.cpp / mpu_main.cpp) so each
 * executable can report its own trap.
 */
#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

extern int main(void);

extern void HardFault_Handler(void);
extern void MemManage_Handler(void);
extern void BusFault_Handler(void);
extern void UsageFault_Handler(void);
extern void SVC_Handler(void);
extern void DebugMon_Handler(void);
extern void PendSV_Handler(void);
extern void SysTick_Handler(void);

/* External interrupt 28 is timer2 on stm32f4.repl (vector 16 + 28 = 44). The
 * admission harness defines it to drive the ISR submission scenario; the MPU
 * probe does not, so the weak alias keeps that build linking and any stray
 * interrupt visibly parked instead of jumping through a null vector. */
void Timer2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

void Reset_Handler(void);
void Default_Handler(void);

__attribute__((section(".isr_vector"), used))
const void* const g_vectors[] = {
    [0] = &_estack,
    [1] = (const void*)Reset_Handler,
    [2] = (const void*)Default_Handler,      /* NMI */
    [3] = (const void*)HardFault_Handler,
    [4] = (const void*)MemManage_Handler,
    [5] = (const void*)BusFault_Handler,
    [6] = (const void*)UsageFault_Handler,
    [11] = (const void*)SVC_Handler,
    [12] = (const void*)DebugMon_Handler,
    [14] = (const void*)PendSV_Handler,
    [15] = (const void*)SysTick_Handler,
    /* Every unused external slot parks in Default_Handler rather than jumping
       through a zero vector, so an unexpected interrupt is a visible hang that
       run.sh reports as a missing RESULT line. */
    [7 ... 10] = (const void*)Default_Handler,
    [13] = (const void*)Default_Handler,
    [16 ... 60] = (const void*)Default_Handler,
    [16 + 28] = (const void*)Timer2_IRQHandler,
};

void Default_Handler(void)
{
    for (;;) {
    }
}

void Reset_Handler(void)
{
    const uint32_t* src = &_sidata;
    uint32_t* dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0U;
    }

    (void)main();
    for (;;) {
    }
}
