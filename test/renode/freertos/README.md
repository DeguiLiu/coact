# FreeRTOS Kernel vendor copy

Source: <https://github.com/FreeRTOS/FreeRTOS-Kernel> tag `V11.1.0`
(202406-LTS), pinned for the Renode PAL fixture.

Only the files needed by `coact::pal::FreeRtos` are copied, unmodified:

- `kernel/`: `tasks.c`, `queue.c`, `list.c`
- `include/`: upstream public headers
- `port/`: `portable/GCC/ARM_CM3/{port.c,portmacro.h}`
- `LICENSE.md`: upstream MIT license

The Cortex-M3 port is used on the Cortex-M4 model on purpose: it avoids the
FPU context code of ARM_CM4F while exercising the same ARMv7-M scheduler,
SVC/PendSV/SysTick and critical-section instructions.

`FreeRTOSConfig.h` lives next to this file and is fixture-owned.
