# coact 热点性能测试过程（火焰图）

> 结论：无 perf/root 权限时，用"穷人采样器"（SIGPROF 定时器 + `backtrace` 采集调用栈）
> 完成热点定位；`--cores 1 --tick-hz 100` 复现 RT-Thread 单核 100 Hz 多线程场景。
> 工具：`src/core/bench_hotpath.cpp` + `tools/flamegraph_svg.py`。

## 1. 构造持续热路径负载

持续跑数秒让采样器有样本可采；把两种派发机制隔离成独立 mode，避免相互干扰：

- `--mode staged`：2 个非 direct AO（Normal + High）+ 2 producer，覆盖
  submit → staging → Dispatcher → Ao::dispatch → action → event_gc；
- `--mode direct`：1 个 direct-eligible AO + 1 producer，覆盖 M1 线程内直派。

## 2. 采集调用栈（穷人采样器）

1. SIGPROF 间隔定时器（默认 1000 Hz，`--hz` 可调 100 Hz）仅在进程占用 CPU 时触发；
2. 处理器内 `backtrace()` 将被中断线程调用栈写入环形缓冲（16384 槽）；
3. 退出时 `dladdr` + `__cxa_demangle` 符号化，按 root→leaf 折叠并去重计数。

## 3. 复现单核 100 Hz 场景

- `--cores 1`：`sched_setaffinity` 把全进程钉到 CPU 0（线程继承亲和性），
  producer 与 Dispatcher 单核抢占，模拟 RT-Thread 单核调度；
- `--tick-hz 100`：单调时钟按 10 ms 量化（Posix PAL `set_tick_hz` 仿真钩子），
  匹配 RT-Thread 100 Hz tick 的时间粒度。

## 4. 生成火焰图并解读

```sh
cmake --build build_bench --target bench_hotpath
./build_bench/src/core/bench_hotpath --mode staged --cores 1 --tick-hz 100 \
    --hz 1000 --seconds 5 --sample out.folded
python3 tools/flamegraph_svg.py out.folded out.svg "coact staged 1c/100Hz"
```

横轴条宽 = 函数累计样本占比（CPU 占用），自宽条向上即调用链。

## 5. 结果与优化落点

- 单核 100 Hz staged（-O2）：condvar 唤醒+等待约 31%、`clock_gettime` 约 14%；
  多核：EventPool `std::mutex` 约 47%。
- 已落地：Dispatcher 仅 idle 时唤醒（signal 调用 -28%）、队列 cell 32 字节对齐。
- 待做：EventPool 无锁 free-list（设计 §6.4 原子倾向）。

## 局限

- Debug 构建虚高框架占比，结论以 `-O2` 为准；
- `backtrace` 需 `-g -fno-omit-frame-pointer -rdynamic`。
