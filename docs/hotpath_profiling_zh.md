# coact 热点函数测试方法（火焰图）

> 结论：无 perf/root 权限时用"穷人采样器"（SIGPROF 定时器 + `backtrace`）做进程级调用栈采样，
> 折叠为火焰图定位热点；`--cores 1 --tick-hz 100` 复现 RT-Thread 单核 100 Hz 场景。

## 工具

- `src/core/bench_hotpath.cpp`：持续热路径基准 + 内置采样器
- `tools/flamegraph_svg.py`：折叠栈 → SVG 火焰图

## 采样原理

1. SIGPROF 间隔定时器（默认 1000 Hz，可 `--hz`）仅在进程占用 CPU 时触发；
2. 处理器内 `backtrace()` 记录被中断线程调用栈到环形缓冲（16384 槽）；
3. 退出时 `dladdr` + `__cxa_demangle` 符号化，按 root→leaf 折叠去重；
4. 折叠栈喂给 `flamegraph_svg.py` 生成 SVG（条宽 ∝ 样本数，函数名 hash 着色）。

## 工作负载（`--mode`）

- `staged`：2 个非 direct AO（Normal + High）+ 2 producer，覆盖 submit → staging →
  Dispatcher → Ao::dispatch → action → event_gc；
- `direct`：1 个 direct-eligible AO + 1 producer，覆盖 M1 线程内直派。

## 单核 100 Hz 场景

- `--cores 1`：`sched_setaffinity` 将全进程钉到 CPU 0（线程继承），producer 与
  Dispatcher 单核抢占，模拟 RT-Thread 单核调度；
- `--tick-hz 100`：单调时钟按 10 ms 量化（Posix PAL 的 `set_tick_hz` 仿真钩子），
  匹配 RT-Thread 100 Hz tick 的时间粒度。

## 用法

```sh
cmake --build build_bench --target bench_hotpath
./build_bench/src/core/bench_hotpath --mode staged --cores 1 --tick-hz 100 \
    --hz 1000 --seconds 5 --sample out.folded
python3 tools/flamegraph_svg.py out.folded out.svg "coact staged 1c/100Hz"
```

## 结果解读

- 横轴条宽 = 该函数累计样本占比（CPU 占用）；从宽条向上即调用链。
- 实测热点（x86_64，-O2，单核 100 Hz staged）：condvar 唤醒+等待约 31%、
  `clock_gettime` 约 14%；多核场景 EventPool `std::mutex` 约 47%。
  优化落地见设计 §14.3 与 `docs/review_report_20260808.md`。

## 局限

- Debug 构建虚高框架占比，结论以 `-O2` 为准；样本率受定时器粒度限制；
- `backtrace` 需要 `-g -fno-omit-frame-pointer -rdynamic`。
