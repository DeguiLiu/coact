// coact::coro single public entry: includes the stable coro headers.
// SPDX-License-Identifier: MIT
// Naming note: the original plan named this component family coact::async;
// it was renamed coact::coro once the scope narrowed to the Linux PAL
// cooperative-coroutine layer (single pthread + stackful ucontext workers
// replacing per-worker pthreads for MCU-fair single-core semantics).
//
// NOT included here: posix.hpp (opt-in escape hatch for blocking IO only -
// see its header comment) and detail/* (internal).
#pragma once

#include "coact/coro/awaitable.hpp"
#include "coact/coro/combinators.hpp"
#include "coact/coro/config.hpp"
#include "coact/pool.hpp"
#include "coact/coro/error.hpp"
#include "coact/coro/scheduler.hpp"
#include "coact/coro/task.hpp"
#include "coact/coro/task_id.hpp"
#include "coact/coro/task_registry.hpp"
#include "coact/coro/version.hpp"
