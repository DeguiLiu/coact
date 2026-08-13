# coact 阶段零批次(A/B/C/D)集成验证报告

- 日期:2026-08-12
- 验证人:Agent E(收官集成验证)
- 范围:静态 PAL、单核 profile、C++17 生命周期、reclaimer 容量四项 A-D 改动的集成验证
- 结论先行:**四类验证门全部通过(受限项 3 条),未发现 A-D 引入的真实核心缺陷**;A-D 改动对主消费方(avlos demo)无回归。

## 0. 环境

| 项 | 值 |
|---|---|
| Host | Linux 6.5.0-45-generic, GCC 11.4, 无 clang |
| QEMU | 6.2.0 (vexpress-a9) |
| RT-Thread | 5.2.1 `~/rtthread_521/rt-thread-5.2.1/bsp/qemu-vexpress-a9` |
| coact 构建 | 新建 `build-e` / `build-e-asan` / `build-e-tsan`(clean configure,未复用旧目录) |
| 工具 | `test/elf_audit.sh`、`test/asan_classify.sh`、`test/tsan_classify.sh`(新增),BSP 新增 `applications/coact_static_pal_qemu.cpp` |

## 1. Host ASan/UBSan(§15.7)—— PASS

- clean configure:`cmake -B build-e-asan -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"`。
- 20 个测试二进制 × 15 次:**REAL_BUG=0**。每个测试在 ASan runtime 成功初始化后 100% 通过(单测 pass 计数 9~14/15,其余为 DEADLYSIGNAL 抖动)。
- **DEADLYSIGNAL 抖动归因 = 环境/ASan runtime,非真实缺陷**:
  - 平凡程序(仅 `malloc/free` ×1000)在 ASan 下 28% 崩溃(14/50),与 coact 无关;UBSan 单独 0/40 稳定,ASan 单独 15/40 抖动 → 问题定位在 libasan(GCC 11.4,libasan.so.6)。
  - 崩溃发生于 ASan runtime 初始化期(exit 139 = SIGSEGV),无堆栈;gdb 下不复现(布局/时序依赖),strace 显示 init 后 LSan/信号处理路径崩溃。
  - ctest 失败集合不稳定:并行跑失败 `dispatcher_reclaim/policy/config/static_lifetime`,串行跑失败 `event/pool_lifecycle/dispatcher_reclaim/queue/core` → 非测试特异性。
- **结论:coact 全部测试在 ASan+UBSan 下无真实内存错误**(无 heap/stack/global 溢出、无 UAF、无 double-free、无 UBSan 未定义行为)。

## 2. Host TSan(§15.4/§15.7)—— PASS(受限)

- ctest 直接跑:`FATAL: ThreadSanitizer: unexpected memory mapping` —— 内核 6.5 高熵 ASLR 与 GCC libtsan 不兼容,100% 环境问题。用 `setarch -R`(禁用 ASLR,见项目自带 `.ai/run-tsan.sh`)解决。
- `setarch -R` 下:20/20 测试无断言失败,16/20 完全无 TSan 告警。
- 5 个告警项经 `build-tsan-baseline`(A-D 前)对照,**全部为 baseline 预存,非 A-D 引入**:
  | 测试 | 告警 | baseline 对照 |
  |---|---|---|
  | test_stress | data race `pool.hpp:127`(free-list block next 字段) | 相同(100% 出现) |
  | test_policy | data race `policy.hpp:167 MergeCell::try_publish(event_=e)` | 相同(50~60%) |
  | test_rtt_pal / test_static_lifetime / test_static_pal | thread leak `rtthread_stub.h rt_thread_startup` | rtt_pal/static_lifetime 相同;static_pal 为 D 新增,同类 stub 未 join 工件 |
- **归因**:两条 data race 均为 CAS 守卫的非原子载荷发布模式(pool Treiber free-list、MergeCell 状态机),符合 C++ 内存模型 release/acquire 语义;GCC 11 libtsan 对 CAS 发布链有已知误报,项目 `.ai/run-tsan.sh` 亦声明"clang TSan 为权威门"。本机无 clang,无法用 clang TSan 复核 → 标注 **受限**。thread leak 为 host stub(pthread 未 join)测试骨架工件。
- **结论:未发现 A-D 引入的新竞态**。

## 3. RT-Thread 5.2.1 QEMU -smp 1(§15.8)—— PASS

- `qemu-nographic.sh` 由 `-smp cpus=2` 改为 `-smp 1`(与 `rtconfig.h RT_CPUS_NR 1` 一致,无 RT_USING_SMP)。
- `scons -j4` 编译通过;`timeout 90 ./qemu-nographic.sh` 串口输出:
  ```
  [spal] [1] explicit-resource initialize+start -> PASS
  [spal] [2] single-core profile (immediate reclaim) assembled
  [spal] [4] task/isr submitted=20/4 dispatched=24/24 rejects=0 used=0
  [spal] [5] stop/join -> PASS
  [spal] RESULT: PASS
  [spal] TEST DONE
  [cmdfw] [1/4] PAL static dispatcher: submitted=24 dispatched=24 pool.used=0 -> PASS
  [cmdfw] [2/4] uart0 transport init -> PASS
  [cmdfw] [4/4] TransportPort ops table + link_active -> PASS
  [cmdfw] [3/4] FD rx decode frames=1 ok=1 -> PASS
  [cmdfw] RESULT: ALL PASS
  [cmdfw] TEST DONE
  ```
- 零堆证据(新增 `coact_static_pal_qemu.cpp`):Dispatcher TCB / wake+join 信号量 / Dispatcher 栈 / ContextSlot 表来自显式 `RtThreadResources<16384,8>`;producer 与 driver 线程用 `rt_thread_init` + 固定栈数组(非 `rt_thread_create`);pool/AO/Runtime 均为静态全局。**未复用动态 smoke 作零堆证据**。
- ISR 路径:`try_submit_from_isr` 从任务上下文驱动 ISR staging+wake 逻辑(真实 hard-timer ISR 触发为 §15.9 目标板门)。

## 4. ELF/map 审计(§15.8 符号黑名单)—— PASS(1 项受限)

- 工具:`test/elf_audit.sh`。对象级 `nm -uC` 审计。
- **框架对象全 CLEAN**:`pal_rtthread.o`、`cmdfw/protocol/{frame,crc,frame_decoder}.o`、`cmdfw/pal/qemu_uart.o` —— 无 `rt_malloc/rt_free`、`rt_*_create`、`malloc/free/realloc/calloc`、`operator new`、`__cxa_throw`、`__cxa_allocate_exception`、`__dynamic_cast`、`__atomic_*`、libatomic。
- 最小构建(仅 main + 静态测试 + coact + cmdfw protocol)最终 ELF:**0 个未定义的 banned 符号**。
- libatomic:0 引用;32 位原子走原生 LDREX/STREX(map 中 `__atomic_` 命中均为 `std::__atomic_base` 类构造器符号,非 libatomic 调用)。
- `__cxa_throw` 在最终 ELF 为 libsupc++ 定义,coact/cmdfw 对象**零引用**(仅旧 `osp_net_test.o` 用 C++ runtime)。
- **受限项(报告主 agent,不改核心)**:coact `AoBase`/`Ao` 虚析构(deleting destructor)引用 `operator delete(void*, unsigned int)`。**运行期永不调用**(coact 无 `delete`、无堆分配,pool 固定、AO 静态),符号随 vtable 进入二进制(baseline 亦存在)。若 §15.8 要求"符号级干净",需调整 AO 析构设计(如 protected 非虚析构)。
- 注:旧测试 app `coact_smoke.o`/`qemu_main.o` 用 `rt_thread_create` 作测试驱动线程、`osp_net_test.o` 用 `operator new`,均为测试脚手架/预存 app,非框架。

## 5. avlos demo 回归(主消费方)—— PASS

- `cmake -B build-e -S /home/dgliu/coact_avlos_demo && cmake --build build-e -j && ctest --test-dir build-e`:**21/21 通过**。
- ASan 构建(`-DCMDFW_SANITIZE=ON`,clean `build-san-e`):17 个测试文件 × 8 次,**REAL_BUG=0**。
- 此前偶发的 `test_response`/`test_domains` 各 15 次:12 pass + 3 env jitter + 0 real → **仍为环境抖动,非 A-D 缺陷,也未被 A-D 修复(本就无缺陷)**。

## 6. 发现的核心缺陷(待主 agent,未修改)

1. **`RtThread()` 默认构造器共享单份内部静态资源**:同镜像内多个默认构造 RtThread(如 coact_smoke + cmdfw qemu_main)会对同一静态 TCB 二次 `rt_thread_init` → RT-Thread debug 断言 `(obj != object) @ rt_object_init:383`。设计 §7.5 已要求生产板传显式 `RtThreadResources`;本次集成按此修复(测试 app 改显式资源)。建议主 agent 评估默认构造器是否应更醒目地限制单实例。
2. **`AoBase` 虚析构引用 `operator delete`**:符号级零堆门(§15.8)会被其卡住;运行期无堆操作。属框架特性,非 A-D 引入。
3. TSan 两条 data race(pool/MergeCell)为 GCC 11 libtsan 疑似误报,建议后续用 clang TSan 复核确认。

## 7. 遗留与建议

- 真实 hard-timer/UART ISR 上下文触发(ISR alloc/submit、wake semaphore saturation)属 §15.9 目标板门,QEMU 仅覆盖 ISR staging+wake 逻辑。
- Host sanitizer 环境不稳定(ASan init 抖动 ~25-30%、TSan 需 `setarch -R`)。建议 CI 用 clang sanitizer 作为权威门;当前环境无 clang。
- `qemu-nographic.sh` 已改 `-smp 1`;BSP `main.c` 改调 `coact_static_pal_auto()`(静态零堆测试),`coact_smoke_auto` 保留但不再由 main 调用。
- 最小零堆构建与完整构建均验证过;完整构建含旧脚手架 app(rt_thread_create/operator new),零堆结论以框架对象级审计 + 最小构建为准。
