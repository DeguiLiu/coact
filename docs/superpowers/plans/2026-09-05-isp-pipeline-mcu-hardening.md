# ISP Pipeline MCU Hardening Implementation Plan

**Goal:** Close the remaining RT-Thread static-resource, ISR-observability, and watermark-telemetry gaps in `isp_pipeline`.

**Architecture:** Keep the existing AO/worker topology. Move long-lived demo objects into function-static storage, exercise the existing ISR submission contract through a bounded probe, and expose sampling counters through the existing Monitor snapshot.

**Tech Stack:** C++17, coact Runtime/Monitor, RT-Thread PAL, host CMake/CTest, Renode smoke.

### Task 1: Add regression coverage

- [x] Add assertions for static resource placement and ISR submission observability.
- [x] Run focused tests and capture the expected failures before implementation.

### Task 2: Harden demo storage

- [x] Move AO, Runtime, DDR, and worker lifetime objects to static storage.
- [x] Preserve initialization order and cleanup order.

### Task 3: Add ISR and watermark metrics

- [x] Submit one bounded ISR-path event and assert delivery/trace accounting.
- [x] Record watermark sampling and diagnostic drop snapshots.

### Task 4: Verify and document

- [x] Run host, RTT stub, Renode, and repeated timer tests.
- [x] Update the functional report and resource budget.
