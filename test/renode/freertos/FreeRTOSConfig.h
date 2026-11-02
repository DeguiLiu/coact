// FreeRTOS configuration for the coact Renode fixture (Cortex-M4 model,
// Cortex-M3 port). Static allocation only; every callback the kernel requires
// in that mode is provided by freertos_main.cpp.
// SPDX-License-Identifier: MIT
#pragma once

#define configUSE_PREEMPTION 1
#define configNUMBER_OF_CORES 1
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 8
#define configMINIMAL_STACK_SIZE 128
#define configTOTAL_HEAP_SIZE 0
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configUSE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configUSE_TASK_NOTIFICATIONS 1
#define configUSE_TIMERS 0
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_MALLOC_FAILED_HOOK 0
#define configUSE_TRACE_FACILITY 0
#define configQUEUE_REGISTRY_SIZE 0
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_16_BIT_TICKS 0
#define configCPU_CLOCK_HZ 84000000UL
#define configPRIO_BITS 4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_vTaskPrioritySet 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1

/* A failed assertion means the fixture must stop loudly, not continue with a
   broken kernel. The harness treats "no RESULT line" as a failure. */
#define configASSERT(x)             \
    do {                            \
        if ((x) == 0) {             \
            for (;;) { }            \
        }                           \
    } while (0)
