# Vocabulary 类型统一实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (recommended) or superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** 统一 `TargetId`、`TaskSlotId` 到 `NewType`，并让 `Expected` 通过 `vocabulary.hpp` 提供，同时保持现有公共 include 和兼容访问。

**Architecture:** `NewType` 增加 `raw()` 兼容访问；两个 ID 改成带独立 Tag 的别名。`Expected` 的唯一实现移动到 `vocabulary.hpp`，`expected.hpp` 变成转发头。函数指针、`std::array`、领域 guard 与事件布局不变。

**Tech Stack:** C++17、CMake/CTest、现有 coact 测试框架、POSIX/RT-Thread stub 示例构建。

---

### Task 1: 先增加统一类型契约测试

**Files:**
- Modify: `src/core/test_vocabulary.cpp`
- Modify: `src/core/test_coro_registry.cpp`

- [ ] **Step 1: Add failing assertions**

在 `test_vocabulary.cpp` 增加 `TargetId`、`TaskSlotId` 的 `raw()`、零开销和强类型静态断言；将 `Expected` 的最小成功/错误路径测试改为直接包含 `coact/vocabulary.hpp` 的编译覆盖。在 `test_coro_registry.cpp` 将 `slot_of(...).value` 改为期望的 `raw()` API。

- [ ] **Step 2: Run focused tests**

Run: `cmake --build build --target test_vocabulary test_expected test_coro_registry`

Expected: compile failure because `NewType::raw()` and the unified vocabulary `Expected` are not yet available under the new assertions.

### Task 2: Implement NewType-based IDs

**Files:**
- Modify: `include/coact/vocabulary.hpp`
- Modify: `include/coact/config.hpp`
- Modify: `include/coact/coro/task_id.hpp`
- Modify: `include/coact/coro/task_registry.hpp`
- Modify: `src/core/test_coro_registry.cpp`

- [ ] **Step 1: Add `NewType::raw()`**

Implement `constexpr T raw() const noexcept { return val_; }` beside `value()`.

- [ ] **Step 2: Replace `TargetId`**

Include `coact/vocabulary.hpp`, define `TargetIdTag`, and replace the hand-written `TargetId` struct with `using TargetId = NewType<uint8_t, TargetIdTag>;`. Keep `kInvalidTarget` and the existing size assertion.

- [ ] **Step 3: Replace `TaskSlotId`**

Define `TaskSlotIdTag` and replace the hand-written struct with `using TaskSlotId = coact::NewType<uint16_t, TaskSlotIdTag>;`. Update internal `.value` reads to `.raw()` or `.value()`.

- [ ] **Step 4: Build focused tests**

Run: `cmake --build build --target test_vocabulary test_coro_registry && ctest --test-dir build -R 'test_vocabulary|test_coro_registry' --output-on-failure`

Expected: PASS.

### Task 3: Move Expected behind vocabulary.hpp

**Files:**
- Modify: `include/coact/vocabulary.hpp`
- Modify: `include/coact/expected.hpp`
- Modify: `src/core/test_vocabulary.cpp`
- Modify: `src/core/test_expected.cpp`

- [ ] **Step 1: Move the existing implementation**

Copy the existing `Expected<V,E>` and `Expected<void,E>` definitions into `vocabulary.hpp` after the existing inline vocabulary types, preserving constructors, storage, assertions, move-only behavior, and public APIs exactly.

- [ ] **Step 2: Make the legacy header a forwarding include**

Replace the duplicate implementation in `expected.hpp` with `#include "coact/vocabulary.hpp"` and retain the public header path.

- [ ] **Step 3: Add direct-entry coverage**

Make `test_vocabulary.cpp` include and instantiate `coact::Expected` through `coact/vocabulary.hpp`; keep `test_expected.cpp` using `coact/expected.hpp` to verify compatibility.

- [ ] **Step 4: Run focused tests**

Run: `cmake --build build --target test_vocabulary test_expected && ctest --test-dir build -R 'test_vocabulary|test_expected' --output-on-failure`

Expected: PASS.

### Task 4: Compile and adjust ISP example consumers

**Files:**
- Modify: any `examples/isp_pipeline/*` file that still relies on removed public ID members

- [ ] **Step 1: Build both ISP variants**

Run: `cmake --build build --target isp_pipeline_demo isp_pipeline_demo_rtt`

Expected: both targets compile and link; if a direct `TargetId` member access fails, replace it with `raw()`/`value()` without changing pipeline behavior.

- [ ] **Step 2: Run the self-verifying host example**

Run: `ctest --test-dir build -R '^isp_pipeline_demo$' --output-on-failure`

Expected: PASS.

### Task 5: Update documentation and run regression verification

**Files:**
- Modify: `docs/cpp_coding_conventions_zh.md`

- [ ] **Step 1: Align documentation**

Update references that describe `Expected` as living only in `expected.hpp`, documenting `vocabulary.hpp` as the canonical implementation and `expected.hpp` as a compatibility include.

- [ ] **Step 2: Run core and example regression tests**

Run: `cmake --build build -j12 && ctest --test-dir build --output-on-failure`

Expected: all previously enabled tests pass, including `isp_pipeline_demo` and its RT-Thread compile gate.
