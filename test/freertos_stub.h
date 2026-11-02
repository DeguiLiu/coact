/**
 * coact FreeRTOS host stub - backs the FreeRTOS Kernel API subset used by
 * coact/pal_freertos.hpp with pthreads. Include via -DCOACT_FREERTOS_STUB.
 *
 * This is a test double, not a FreeRTOS port: it does not schedule by
 * priority, does not emulate tick interrupts, and treats a simulated ISR as a
 * synchronous call inside the calling thread. It does preserve the contracts
 * the PAL depends on: static storage is caller-owned, a task function never
 * returns, semaphores are counting/blocking, and every critical section is
 * process-wide mutually exclusive.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>

#include <atomic>
#include <pthread.h>
#include <sched.h>
#include <time.h>

typedef uint32_t StackType_t;
typedef uint32_t TickType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef void (*TaskFunction_t)(void*);

struct StaticTask_t;
typedef StaticTask_t* TaskHandle_t;
struct StaticSemaphore_t;
typedef StaticSemaphore_t* SemaphoreHandle_t;

#ifndef configNUMBER_OF_CORES
#define configNUMBER_OF_CORES 1
#endif
#ifndef configSUPPORT_STATIC_ALLOCATION
#define configSUPPORT_STATIC_ALLOCATION 1
#endif
#ifndef configUSE_MUTEXES
#define configUSE_MUTEXES 1
#endif
#ifndef configUSE_COUNTING_SEMAPHORES
#define configUSE_COUNTING_SEMAPHORES 1
#endif
#ifndef INCLUDE_vTaskSuspend
#define INCLUDE_vTaskSuspend 1
#endif
#ifndef INCLUDE_vTaskPrioritySet
#define INCLUDE_vTaskPrioritySet 1
#endif
#ifndef INCLUDE_vTaskDelay
#define INCLUDE_vTaskDelay 1
#endif
#ifndef INCLUDE_xTaskGetSchedulerState
#define INCLUDE_xTaskGetSchedulerState 1
#endif
#ifndef INCLUDE_vTaskDelete
#define INCLUDE_vTaskDelete 1
#endif
#ifndef INCLUDE_xTaskGetCurrentTaskHandle
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#endif
#ifndef configUSE_16_BIT_TICKS
#define configUSE_16_BIT_TICKS 0
#endif
#ifndef configMAX_PRIORITIES
#define configMAX_PRIORITIES 8
#endif
#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 1000
#endif

#define pdTRUE ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdPASS pdTRUE
#define pdFAIL pdFALSE
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFUL)

#define taskSCHEDULER_NOT_STARTED ((BaseType_t)1)
#define taskSCHEDULER_RUNNING ((BaseType_t)2)
#define taskSCHEDULER_SUSPENDED ((BaseType_t)0)

struct StaticTask_t {
  pthread_t thread{};
  TaskFunction_t fn = nullptr;
  void* arg = nullptr;
  StackType_t* stack = nullptr;
  uint32_t depth = 0U;
  UBaseType_t prio = 0U;
  bool in_use = false;
};

struct StaticSemaphore_t {
  pthread_mutex_t mtx{};
  pthread_cond_t cond{};
  uint32_t count = 0U;
  uint32_t max = 1U;
  bool is_mutex = false;
  bool inited = false;
  pthread_t owner{};
};

/* ---- Fault injection (matches test/rtthread_stub.h) -------------------- */
inline bool& stub_freertos_fail_sem() noexcept {
  static bool v = false;
  return v;
}
inline bool& stub_freertos_fail_task() noexcept {
  static bool v = false;
  return v;
}
inline void stub_freertos_reset_faults() noexcept {
  stub_freertos_fail_sem() = false;
  stub_freertos_fail_task() = false;
}

/* ---- ISR simulation ---------------------------------------------------- */
inline thread_local bool g_stub_freertos_in_isr = false;
inline bool freertos_stub_in_isr() noexcept {
  return g_stub_freertos_in_isr;
}
inline void stub_freertos_set_in_isr(bool v) noexcept {
  g_stub_freertos_in_isr = v;
}

/* ---- Critical sections: one process-wide recursive mutex ---------------- */
inline pthread_mutex_t& stub_freertos_critical() noexcept {
  static pthread_mutex_t m;
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, []() {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m, &attr);
    pthread_mutexattr_destroy(&attr);
  });
  return m;
}
inline thread_local int g_stub_freertos_critical_depth = 0;

#define taskENTER_CRITICAL()                           \
  do {                                                 \
    if (0 == ::g_stub_freertos_critical_depth++) {     \
      pthread_mutex_lock(&::stub_freertos_critical()); \
    }                                                  \
  } while (0)

#define taskEXIT_CRITICAL()                              \
  do {                                                   \
    if (0 == --::g_stub_freertos_critical_depth) {       \
      pthread_mutex_unlock(&::stub_freertos_critical()); \
    }                                                    \
  } while (0)

inline UBaseType_t portSET_INTERRUPT_MASK_FROM_ISR() noexcept {
  return 1U;
}
inline void portCLEAR_INTERRUPT_MASK_FROM_ISR(UBaseType_t) noexcept {}
inline void portYIELD_FROM_ISR(BaseType_t) noexcept {}
inline void taskYIELD() noexcept {
  sched_yield();
}

/* ---- Tick source ------------------------------------------------------- */
inline uint64_t stub_freertos_now_ticks() noexcept {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
  return (ns * static_cast<uint64_t>(configTICK_RATE_HZ)) / 1000000000ULL;
}
inline TickType_t xTaskGetTickCount() noexcept {
  return static_cast<TickType_t>(stub_freertos_now_ticks());
}
inline TickType_t xTaskGetTickCountFromISR() noexcept {
  return xTaskGetTickCount();
}

inline void stub_freertos_sleep_ticks(TickType_t ticks) noexcept {
  if (0U == ticks) {
    sched_yield();
    return;
  }
  const uint64_t ns = (static_cast<uint64_t>(ticks) * 1000000000ULL) / configTICK_RATE_HZ;
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}
inline void vTaskDelay(TickType_t ticks) noexcept {
  stub_freertos_sleep_ticks(ticks);
}

/* ---- Tasks ------------------------------------------------------------- */
inline thread_local TaskHandle_t g_stub_freertos_current = nullptr;

/* Lets a test model "start_dispatcher() before vTaskStartScheduler()": the
   real kernel returns NULL from xTaskGetCurrentTaskHandle() in that state. */
inline bool& stub_freertos_scheduler_running() noexcept {
  static bool v = true;
  return v;
}

inline BaseType_t xTaskGetSchedulerState() noexcept {
  return stub_freertos_scheduler_running() ? taskSCHEDULER_RUNNING : taskSCHEDULER_NOT_STARTED;
}

inline StaticTask_t& stub_freertos_main_task() noexcept {
  static StaticTask_t main_task;
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, []() {
    main_task.thread = pthread_self();
    main_task.in_use = true;
  });
  return main_task;
}

inline void* stub_freertos_trampoline(void* p) noexcept {
  StaticTask_t* task = static_cast<StaticTask_t*>(p);
  g_stub_freertos_current = task;
  task->fn(task->arg);
  g_stub_freertos_current = nullptr;
  return nullptr;
}

inline TaskHandle_t xTaskCreateStatic(TaskFunction_t fn, const char*, uint32_t depth, void* arg, UBaseType_t prio,
                                      StackType_t* stack, StaticTask_t* tcb) noexcept {
  if (nullptr == fn || nullptr == stack || nullptr == tcb) {
    return nullptr;
  }
  if (stub_freertos_fail_task()) {
    stub_freertos_fail_task() = false;
    return nullptr;
  }
  tcb->fn = fn;
  tcb->arg = arg;
  tcb->stack = stack;
  tcb->depth = depth;
  tcb->prio = prio;
  tcb->in_use = true;
  if (0 != pthread_create(&tcb->thread, nullptr, &stub_freertos_trampoline, tcb)) {
    tcb->in_use = false;
    return nullptr;
  }
  return tcb;
}

inline TaskHandle_t xTaskGetCurrentTaskHandle() noexcept {
  if (!stub_freertos_scheduler_running()) {
    return nullptr;
  }
  if (nullptr != g_stub_freertos_current) {
    return g_stub_freertos_current;
  }
  return &stub_freertos_main_task();
}

inline void vTaskPrioritySet(TaskHandle_t task, UBaseType_t prio) noexcept {
  if (nullptr == task) {
    task = xTaskGetCurrentTaskHandle();
  }
  task->prio = prio;
}

/* A real FreeRTOS task function never returns; the stub mirrors that by
   terminating the pthread instead of unwinding into the caller. */
inline void vTaskDelete(TaskHandle_t task) noexcept {
  if ((nullptr == task) || (task == xTaskGetCurrentTaskHandle())) {
    pthread_exit(nullptr);
  }
  task->in_use = false;
}

inline void vTaskSuspend(TaskHandle_t) noexcept {
  /* No test suspends a thread; block forever like the real kernel. */
  for (;;) {
    struct timespec ts{1, 0};
    nanosleep(&ts, nullptr);
  }
}

/* ---- Semaphores -------------------------------------------------------- */
inline void stub_freertos_sem_init(StaticSemaphore_t* s, uint32_t max, uint32_t initial, bool is_mutex) noexcept {
  pthread_mutex_init(&s->mtx, nullptr);
  pthread_cond_init(&s->cond, nullptr);
  s->count = initial;
  s->max = max;
  s->is_mutex = is_mutex;
  s->inited = true;
  s->owner = pthread_t{};
}

inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* storage) noexcept {
  if (nullptr == storage || stub_freertos_fail_sem()) {
    stub_freertos_fail_sem() = false;
    return nullptr;
  }
  stub_freertos_sem_init(storage, 1U, 0U, false);
  return storage;
}

inline SemaphoreHandle_t xSemaphoreCreateCountingStatic(UBaseType_t max, UBaseType_t initial,
                                                        StaticSemaphore_t* storage) noexcept {
  if (nullptr == storage || stub_freertos_fail_sem()) {
    stub_freertos_fail_sem() = false;
    return nullptr;
  }
  stub_freertos_sem_init(storage, static_cast<uint32_t>(max), static_cast<uint32_t>(initial), false);
  return storage;
}

inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) noexcept {
  if (nullptr == storage || stub_freertos_fail_sem()) {
    stub_freertos_fail_sem() = false;
    return nullptr;
  }
  stub_freertos_sem_init(storage, 1U, 1U, true);
  return storage;
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) noexcept {
  if (nullptr == sem || !sem->inited) {
    return pdFALSE;
  }
  pthread_mutex_lock(&sem->mtx);
  if (0U == ticks) {
    if (0U == sem->count) {
      pthread_mutex_unlock(&sem->mtx);
      return pdFALSE;
    }
  } else if (static_cast<TickType_t>(portMAX_DELAY) == ticks) {
    while (0U == sem->count) {
      pthread_cond_wait(&sem->cond, &sem->mtx);
    }
  } else {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    const uint64_t ns = (static_cast<uint64_t>(ticks) * 1000000000ULL) / configTICK_RATE_HZ;
    deadline.tv_sec += static_cast<time_t>(ns / 1000000000ULL);
    deadline.tv_nsec += static_cast<long>(ns % 1000000000ULL);
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }
    while (0U == sem->count) {
      if (ETIMEDOUT == pthread_cond_timedwait(&sem->cond, &sem->mtx, &deadline)) {
        pthread_mutex_unlock(&sem->mtx);
        return pdFALSE;
      }
    }
  }
  --sem->count;
  if (sem->is_mutex) {
    sem->owner = pthread_self();
  }
  pthread_mutex_unlock(&sem->mtx);
  return pdTRUE;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) noexcept {
  if (nullptr == sem || !sem->inited) {
    return pdFALSE;
  }
  pthread_mutex_lock(&sem->mtx);
  if (sem->is_mutex && !pthread_equal(sem->owner, pthread_self())) {
    pthread_mutex_unlock(&sem->mtx);
    return pdFALSE;
  }
  if (sem->count >= sem->max) {
    pthread_mutex_unlock(&sem->mtx);
    return pdFALSE;
  }
  ++sem->count;
  if (sem->is_mutex) {
    sem->owner = pthread_t{};
  }
  pthread_cond_signal(&sem->cond);
  pthread_mutex_unlock(&sem->mtx);
  return pdTRUE;
}

inline BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t* woken) noexcept {
  if (nullptr != woken) {
    *woken = pdFALSE;
  }
  return xSemaphoreGive(sem);
}

inline void vSemaphoreDelete(SemaphoreHandle_t sem) noexcept {
  if (nullptr == sem || !sem->inited) {
    return;
  }
  pthread_mutex_destroy(&sem->mtx);
  pthread_cond_destroy(&sem->cond);
  sem->inited = false;
}
