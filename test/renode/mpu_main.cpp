/*
 * Cortex-M4 MPU capability matrix for Renode (stage-0 platform evidence).
 *
 * Design 8.1 requires the platform gate to answer, per capability, whether the
 * target actually enforces an access attribute - not whether the register
 * accepted a write. Each probe below therefore pairs a risky access with a
 * control access, so a fault can be attributed to the configuration under test
 * instead of to the model refusing RAM in general:
 *
 *   no-access write / read      + control write with the MPU disabled
 *   privileged write to RO      + privileged read from the same RO region
 *   execute from an XN region   + execute the same bytes from a non-XN region
 *   overlapping regions         + the same access after the winner is disabled
 *   stack-guard overflow        + a separate handler stack so the fault frame
 *                                 itself does not land inside the guard
 *   unprivileged access         + the same access in privileged mode
 *
 * Results are reported in two classes. ASSERT lines validate the harness and
 * capabilities already known to work; a FAIL there is a regression. CAP lines
 * report architecture conformance the design depends on, and mpu.sh fails when
 * a capability marked required is NOT_ENFORCED, because that is the finding the
 * gate exists to surface.
 *
 * Faults are survivable: the handler records CFSR/MMFAR, clears the sticky
 * bits, rewrites the stacked PC to the stacked LR (every risky access lives in
 * a noinline callee, so that address is a valid continuation) and lets the
 * hardware perform a normal exception return.
 */
#include <cstdint>

#include "output.hpp"

namespace {

constexpr uintptr_t kMpuType = 0xE000ED90UL;
constexpr uintptr_t kMpuCtrl = 0xE000ED94UL;
constexpr uintptr_t kMpuRnr = 0xE000ED98UL;
constexpr uintptr_t kMpuRbar = 0xE000ED9CUL;
constexpr uintptr_t kMpuRasr = 0xE000EDA0UL;
constexpr uintptr_t kScbShcsr = 0xE000ED24UL;
constexpr uintptr_t kScbCfsr = 0xE000ED28UL;
constexpr uintptr_t kScbHfsr = 0xE000ED2CUL;
constexpr uintptr_t kScbMmfar = 0xE000ED34UL;

/* ARMv7-M AP[2:0] encodings used below. */
constexpr uint32_t kApNoAccess = 0U;        /* no access, either mode */
constexpr uint32_t kApPrivRwOnly = 1U;      /* privileged RW, unpriv none */
constexpr uint32_t kApPrivRoOnly = 5U;      /* privileged RO, unpriv none */
constexpr uint32_t kApFullRw = 3U;          /* privileged and unpriv RW */
constexpr uint32_t kApBothRo = 6U;          /* read-only for both modes */

/* Probe targets: fixed 1 KB slots in the middle of SRAM, far from .bss and
   from the MSP stack at the top of RAM. */
constexpr uintptr_t kSlotNoAccess = 0x20010000UL;
constexpr uintptr_t kSlotXn = 0x20011000UL;
constexpr uintptr_t kSlotExec = 0x20012000UL;
constexpr uintptr_t kSlotOverlap = 0x20013000UL;
constexpr uintptr_t kSlotPlain = 0x20014000UL;
constexpr uint32_t kSlotLog2 = 10U;         /* 1 KB */
/* Unprivileged execution needs its own regions: the background map is
   privileged-only, so without them the first fetch faults and a data-access
   probe would never run. Flash is the 128 KB the linker script declares; the
   MSP window is the 64 KB below the initial stack pointer. */
constexpr uintptr_t kFlashBase = 0x08000000UL;
constexpr uint32_t kFlashLog2 = 17U;
constexpr uintptr_t kStackWinBase = 0x20020000UL;
constexpr uint32_t kStackWinLog2 = 16U;

constexpr uintptr_t kPattern = 0xDEADBEEFUL;

inline volatile uint32_t& reg(uintptr_t address) noexcept
{
    return *reinterpret_cast<volatile uint32_t*>(address);  // NOLINT
}

inline void dsb_isb() noexcept
{
    __asm__ volatile("dsb\n\tisb" ::: "memory");
}

inline uint32_t rasr_of(uint32_t size_log2, uint32_t ap, bool xn) noexcept
{
    return (1U << 0)                       /* ENABLE */
           | ((size_log2 - 1U) << 1)       /* SIZE */
           | (ap << 24)                    /* AP */
           | (xn ? (1U << 28) : 0U);       /* XN */
}

inline void mpu_region(uint32_t num, uintptr_t base, uint32_t rasr) noexcept
{
    reg(kMpuRnr) = num;
    reg(kMpuRbar) = base;
    reg(kMpuRasr) = rasr;
    dsb_isb();
}

inline void mpu_region_off(uint32_t num) noexcept
{
    reg(kMpuRnr) = num;
    reg(kMpuRasr) = 0U;
    dsb_isb();
}

inline void mpu_enable(bool privdefena) noexcept
{
    reg(kMpuCtrl) = (privdefena ? 5U : 1U);
    dsb_isb();
}

inline void mpu_disable() noexcept
{
    reg(kMpuCtrl) = 0U;
    dsb_isb();
}

// ---------------------------------------------------------------------------
// Fault state shared with the handlers.
// ---------------------------------------------------------------------------
volatile uint32_t g_fault_kind = 0U;   /* 0 none, 1 memmanage, 2 bus, 3 usage,
                                          4 hardfault */
volatile uint32_t g_fault_cfsr = 0U;
volatile uint32_t g_fault_mmfar = 0U;
volatile uintptr_t g_resume_pc = 0U;   /* 0 => resume at the stacked LR */
/* Unprivileged-probe state. The target cannot print while nPRIV is set: with
   PRIVDEFENA the background map only covers privileged accesses, so the UART
   itself would fault. The handler therefore hands privilege back and records
   the one fact the probe needs - whether the slot write was what faulted. */
volatile uint32_t g_arm_unpriv = 0U;
volatile uint32_t g_unpriv_saw_slot_write = 0U;
/* Stack-guard probe state: armed while recursion runs, and set by the handler
   when the overflow actually hit the guard region. */
volatile uint32_t g_guard_arm = 0U;
volatile uint32_t g_guard_resumed = 0U;
volatile uint32_t g_sink = 0U;

void fault_clear() noexcept
{
    g_fault_kind = 0U;
    g_fault_cfsr = 0U;
    g_fault_mmfar = 0U;
    g_resume_pc = 0U;
}

bool faulted() noexcept
{
    return 0U != g_fault_kind;
}

/* CFSR bit meanings this fixture asserts on. */
constexpr uint32_t kIaccviol = 1U << 0;
constexpr uint32_t kDaccviol = 1U << 1;
constexpr uint32_t kMmarvalid = 1U << 7;

// ---------------------------------------------------------------------------
// Risky accesses, each in its own noinline callee so the stacked LR is a valid
// resume point in the caller.
// ---------------------------------------------------------------------------
inline uint32_t control_get() noexcept
{
    uint32_t value = 0U;
    __asm__ volatile("mrs %0, control" : "=r"(value));
    return value;
}

inline void control_set(uint32_t value) noexcept
{
    __asm__ volatile("msr control, %0\n\tisb" : : "r"(value) : "memory");
}

[[gnu::noinline]] void probe_write(uintptr_t address, uint32_t value)
{
    reg(address) = value;
    dsb_isb();
}

[[gnu::noinline]] uint32_t probe_read(uintptr_t address)
{
    const uint32_t value = reg(address);
    dsb_isb();
    return value;
}

[[gnu::noinline]] void probe_execute(uintptr_t code_address)
{
    using Fn = void (*)();
    const Fn fn = reinterpret_cast<Fn>(code_address | 1U);  // NOLINT
    fn();
}

/* Drop to unprivileged thread mode, touch the privileged-only slot, then make
   sure privilege is back even if the model does not trap MSR CONTROL. A fault
   inside either call resumes in main, because both are noinline. */
[[gnu::noinline]] void unpriv_write_probe()
{
    control_set(control_get() | 1U);
    probe_write(kSlotPlain, 0x12345678UL);
    while (0U != (control_get() & 1U)) {
        control_set(control_get() & ~1U);
    }
}

/* Consumes ~100 bytes of stack per level until the guard region catches it.
   O0 and the use of `pad` after the recursive call are both deliberate: at -Os
   the tail call turns this into a constant-stack loop and the guard is never
   reached, which made the probe report a false NOT_ENFORCED. */
[[gnu::noinline, gnu::optimize("O0")]] uint32_t recurse_deep(
    uint32_t depth, volatile uint32_t* sink)
{
    volatile uint32_t pad[24];
    pad[0] = depth;
    pad[23] = depth ^ 0x5A5A5A5AUL;
    uint32_t acc = pad[0] + pad[23];
    if (depth < 30U) {          /* 30 * ~100 B > the 1792 B usable stack */
        acc += recurse_deep(depth + 1U, sink);
    }
    *sink += acc;
    return acc;
}

// ---------------------------------------------------------------------------
// Reporting.
// ---------------------------------------------------------------------------
int g_pass = 0;
int g_fail = 0;
int g_cap_total = 0;
int g_cap_enforced = 0;
int g_req_total = 0;
int g_req_enforced = 0;

void record(bool ok, const char* name)
{
    harness::puts(ok ? "PASS " : "FAIL ");
    harness::puts(name);
    harness::newline();
    if (ok) {
        ++g_pass;
    }
    else {
        ++g_fail;
    }
}

void record_u64(const char* name, uint64_t got, uint64_t want)
{
    if (got == want) {
        record(true, name);
        return;
    }
    harness::puts("FAIL ");
    harness::puts(name);
    harness::puts(" got=");
    harness::put_u64(got);
    harness::puts(" want=");
    harness::put_u64(want);
    harness::newline();
    ++g_fail;
}

/* A capability finding: enforced or not, and whether the design requires it. */
void record_cap(const char* name, bool enforced, bool required)
{
    harness::puts("CAP ");
    harness::puts(name);
    harness::puts(enforced ? " ENFORCED" : " NOT_ENFORCED");
    harness::puts(required ? " required" : " informational");
    harness::newline();
    ++g_cap_total;
    if (enforced) {
        ++g_cap_enforced;
    }
    if (required) {
        ++g_req_total;
        if (enforced) {
            ++g_req_enforced;
        }
    }
}

void report_fault(const char* name) noexcept
{
    harness::puts("MPU FAULT ");
    harness::puts(name);
    harness::puts(" CFSR=");
    harness::put_hex32(g_fault_cfsr);
    harness::puts(" MMFAR=");
    harness::put_hex32(g_fault_mmfar);
    harness::newline();
}

// ---------------------------------------------------------------------------
// Stack selection.
// ---------------------------------------------------------------------------
alignas(2048) uint8_t g_stack_area[2048];
constexpr uint32_t kGuardBytes = 256U;

/* Shared by the fall-through path and by the handler's resume path. */
void report_guard_result(bool guard_hit) noexcept
{
    const uintptr_t guard_base = reinterpret_cast<uintptr_t>(&g_stack_area[0]);  // NOLINT
    report_fault("stack_guard");
    record_cap("MPU.stack_guard_catches_overflow", guard_hit, true);
    record_cap("MPU.stack_guard_mmfar_in_guard",
               guard_hit && (g_fault_mmfar >= guard_base)
                   && (g_fault_mmfar < (guard_base + kGuardBytes)),
               true);
    harness::puts("MPU.info guard_base=");
    harness::put_hex32(static_cast<uint32_t>(guard_base));
    harness::puts(" guard_mmfar=");
    harness::put_hex32(g_fault_mmfar);
    harness::puts(" sink=");
    harness::put_u64(g_sink);
    harness::newline();
}

}  // namespace

// ---------------------------------------------------------------------------
// Exception entry: pick the frame the hardware used, then hand over to C.
// `b` (not `bl`) keeps EXC_RETURN in LR, so fault_common's return performs the
// real exception return with the rewritten frame.
// ---------------------------------------------------------------------------
extern "C" void fault_common(uint32_t* frame, uint32_t kind);

extern "C" void __attribute__((naked)) MemManage_Handler(void)
{
    __asm__ volatile("tst lr, #4\n\tite eq\n\tmrseq r0, msp\n\tmrsne r0, psp"
                     "\n\tmovs r1, #1\n\tb fault_common");
}

extern "C" void __attribute__((naked)) BusFault_Handler(void)
{
    __asm__ volatile("tst lr, #4\n\tite eq\n\tmrseq r0, msp\n\tmrsne r0, psp"
                     "\n\tmovs r1, #2\n\tb fault_common");
}

extern "C" void __attribute__((naked)) UsageFault_Handler(void)
{
    __asm__ volatile("tst lr, #4\n\tite eq\n\tmrseq r0, msp\n\tmrsne r0, psp"
                     "\n\tmovs r1, #3\n\tb fault_common");
}

extern "C" void __attribute__((naked)) HardFault_Handler(void)
{
    __asm__ volatile("tst lr, #4\n\tite eq\n\tmrseq r0, msp\n\tmrsne r0, psp"
                     "\n\tmovs r1, #4\n\tb fault_common");
}

[[noreturn]] void run_summary();

/* Finishes the run after a guard hit, from handler mode on a fresh stack. */
extern "C" {
[[noreturn]] void guard_tail(void);
}

extern "C" void fault_common(uint32_t* frame, uint32_t kind)
{
    g_fault_kind = kind;
    g_fault_cfsr = reg(kScbCfsr);
    g_fault_mmfar = reg(kScbMmfar);
    /* Write-1-to-clear, or the sticky bit survives and the next probe reports
       a fault that belongs to the previous one. */
    reg(kScbCfsr) = g_fault_cfsr;
    reg(kScbHfsr) = reg(kScbHfsr);
    if (0U != g_arm_unpriv) {
        if ((1U == kind) && (kSlotPlain == g_fault_mmfar)) {
            g_unpriv_saw_slot_write = 1U;
        }
        /* Fixture rule, not a design claim: give thread mode its privilege
           back. CONTROL.nPRIV affects thread mode only, so writing it from the
           handler is legal and survives the exception return. */
        control_set(control_get() & ~1U);
    }
    const uintptr_t guard_base = reinterpret_cast<uintptr_t>(&g_stack_area[0]);
    /* A real overflow often shows up as a stacking error (UsageFault with
       MSTKERR) rather than a plain MemManage: the hardware cannot push the
       exception frame because the thread SP already points into the guard. */
    if ((0U != g_guard_arm) && (g_fault_mmfar >= guard_base)
        && (g_fault_mmfar < (guard_base + kGuardBytes))) {
        g_guard_resumed = 1U;
        /* The hardware could not even stack a return frame (thread SP is in
           the guard), so there is nothing to return to. A real policy would
           reset the faulting thread; the fixture switches to a clean handler
           stack and finishes the run from handler mode. */
        __asm__ volatile("msr msp, %0\n\tisb" : : "r"(
            reinterpret_cast<uintptr_t>(&g_stack_area[0])
            + sizeof(g_stack_area)) : "memory");
        guard_tail();
    }
    /* frame[5] is the stacked LR, frame[6] the stacked PC. Every risky access
       is a callee, so its LR is the instruction after the call. */
    frame[6] = (0U != g_resume_pc)
                   ? static_cast<uint32_t>(g_resume_pc)
                   : frame[5];
}

extern "C" void SVC_Handler(void) { for (;;) {} }
extern "C" void DebugMon_Handler(void) { for (;;) {} }
extern "C" void PendSV_Handler(void) { for (;;) {} }
extern "C" void SysTick_Handler(void) { for (;;) {} }
extern "C" void abort(void) noexcept
{
    harness::puts("RESULT MPU FAIL 0/0\r\n");
    for (;;) {}
}

extern "C" int main(void)
{
    harness::uart_init();
    harness::puts("MPU TYPE DREGION=");
    harness::put_u64((reg(kMpuType) >> 8) & 0xFFU);
    harness::newline();
    record_u64("MPU.dregion", (reg(kMpuType) >> 8) & 0xFFU, 8U);

    /* MemManage/BusFault/UsageFault enabled instead of escalating everything
       to HardFault, so each trap is named and its cause address is valid. */
    reg(kScbShcsr) |= (1UL << 16) | (1UL << 17) | (1UL << 18);

    /* ---- region readback ------------------------------------------------ */
    mpu_region(0U, kSlotNoAccess, rasr_of(kSlotLog2, kApNoAccess, true));
    record_u64("MPU.rasr_readback", reg(kMpuRasr),
               rasr_of(kSlotLog2, kApNoAccess, true));
    mpu_enable(true);
    harness::puts("MPU CTRL=");
    harness::put_hex32(reg(kMpuCtrl));
    harness::newline();

    /* ---- control: plain RAM with the background map --------------------- */
    fault_clear();
    probe_write(kSlotPlain, kPattern);
    record(!faulted(), "MPU.control_plain_write_ok");
    record_u64("MPU.control_plain_readback", probe_read(kSlotPlain), kPattern);

    /* ---- no-access region ------------------------------------------------ */
    fault_clear();
    probe_write(kSlotNoAccess, kPattern);
    const bool na_write = faulted();
    report_fault("noaccess_write");
    record(na_write, "MPU.noaccess_write_faults");
    record_u64("MPU.noaccess_write_kind", g_fault_kind, 1U);
    record(0U != (g_fault_cfsr & kDaccviol), "MPU.noaccess_write_daccviol");
    record(0U != (g_fault_cfsr & kMmarvalid), "MPU.noaccess_write_mmarvalid");
    record_u64("MPU.noaccess_write_mmfar", g_fault_mmfar, kSlotNoAccess);

    fault_clear();
    (void)probe_read(kSlotNoAccess);
    record(faulted(), "MPU.noaccess_read_faults");

    /* ---- control: the same write with the MPU off ----------------------- */
    mpu_disable();
    fault_clear();
    probe_write(kSlotNoAccess, kPattern);
    const bool off_ok = !faulted();
    record(off_ok, "MPU.disabled_write_ok");
    record_u64("MPU.disabled_write_value", probe_read(kSlotNoAccess), kPattern);
    mpu_enable(true);
    /* The fault above came from the region, not from the address being
       unmapped in the model: put the slot back to a known value. */
    mpu_region_off(0U);
    probe_write(kSlotNoAccess, 0U);
    mpu_region(0U, kSlotNoAccess, rasr_of(kSlotLog2, kApNoAccess, true));

    /* ---- capability: privileged write to a read-only region ------------- */
    mpu_region(1U, kSlotNoAccess, rasr_of(kSlotLog2, kApPrivRoOnly, true));
    fault_clear();
    probe_write(kSlotNoAccess, kPattern);
    const bool ro_write_blocked = faulted();
    report_fault("ro_write");
    record_cap("MPU.ro_write_blocked_privileged", ro_write_blocked, true);

    fault_clear();
    const uint32_t ro_value = probe_read(kSlotNoAccess);
    record_cap("MPU.ro_read_allowed_privileged", !faulted(), true);
    harness::puts("MPU.info ro_read=");
    harness::put_hex32(ro_value);
    harness::newline();

    /* ---- capability: execute-never -------------------------------------- */
    /* bx lr; nop - identical bytes in both slots, so the only difference is
       the XN attribute of the region covering them. */
    volatile uint16_t* xn_code = reinterpret_cast<volatile uint16_t*>(kSlotXn);  // NOLINT
    volatile uint16_t* ok_code = reinterpret_cast<volatile uint16_t*>(kSlotExec);  // NOLINT
    mpu_region_off(1U);
    mpu_region(2U, kSlotXn, rasr_of(kSlotLog2, kApFullRw, true));
    mpu_region(3U, kSlotExec, rasr_of(kSlotLog2, kApFullRw, false));
    xn_code[0] = 0x4770U;
    xn_code[1] = 0xBF00U;
    ok_code[0] = 0x4770U;
    ok_code[1] = 0xBF00U;
    dsb_isb();

    fault_clear();
    probe_execute(kSlotExec);
    const bool exec_ram_ok = !faulted();
    record_cap("MPU.ram_execute_allowed_when_not_xn", exec_ram_ok, true);

    fault_clear();
    probe_execute(kSlotXn);
    const bool xn_blocked = faulted();
    report_fault("xn_execute");
    record_cap("MPU.xn_execute_blocked", xn_blocked, true);

    /* ---- capability: overlap priority and region disable ---------------- */
    mpu_region(4U, kSlotOverlap, rasr_of(kSlotLog2, kApFullRw, false));
    mpu_region(5U, kSlotOverlap, rasr_of(kSlotLog2, kApNoAccess, true));
    fault_clear();
    probe_write(kSlotOverlap, kPattern);
    record_cap("MPU.overlap_higher_region_wins", faulted(), true);

    mpu_region_off(5U);
    fault_clear();
    probe_write(kSlotOverlap, kPattern);
    const bool overlap_released = !faulted();
    record_cap("MPU.region_disable_releases_access", overlap_released, true);
    record_u64("MPU.overlap_value_after_disable", probe_read(kSlotOverlap),
               kPattern);

    /* ---- capability: unprivileged access checking ----------------------- */
    /* Informational on purpose: this design protects trusted privileged code,
       so the answer does not gate P3. It does decide whether Renode can ever
       serve as evidence for a user/kernel split. It runs before the guard
       probe, because the guard path finishes from handler mode, where the
       privilege bit has no meaning. */
    mpu_region(7U, kSlotPlain, rasr_of(kSlotLog2, kApPrivRwOnly, false));
    mpu_region(5U, kFlashBase, rasr_of(kFlashLog2, kApBothRo, false));
    mpu_region(4U, kStackWinBase, rasr_of(kStackWinLog2, kApFullRw, false));
    fault_clear();
    probe_write(kSlotPlain, kPattern);
    record_cap("MPU.priv_write_to_priv_only_ok", !faulted(), false);

    g_unpriv_saw_slot_write = 0U;
    g_arm_unpriv = 1U;
    fault_clear();
    unpriv_write_probe();
    g_arm_unpriv = 0U;                    /* thread mode is privileged again */
    const bool unpriv_blocked = (0U != g_unpriv_saw_slot_write);
    report_fault("unpriv_write");
    record_cap("MPU.unpriv_write_blocked", unpriv_blocked, false);

    fault_clear();
    probe_write(kSlotPlain, kPattern);
    record_cap("MPU.priv_write_restored", !faulted(), false);
    mpu_region_off(5U);
    mpu_region_off(4U);

    /* ---- capability: stack guard with a separate handler stack ---------- */
    /* Thread mode runs on PSP over the usable part of g_stack_area; region 6
       makes the 256 bytes below it inaccessible. Handler mode runs on MSP at
       the top of RAM, so an ordinary fault frame never lands in the guard -
       the precondition design 5.1 puts on any guard region. A real overflow is
       different: the thread SP itself ends up inside the guard, exception
       stacking fails (UsageFault + MSTKERR) and there is no frame to return
       to, so guard_tail() finishes the run from handler mode. */
    const uintptr_t guard_base = reinterpret_cast<uintptr_t>(&g_stack_area[0]);  // NOLINT
    const uintptr_t stack_top = guard_base + sizeof(g_stack_area);
    mpu_region(6U, guard_base, rasr_of(8U, kApNoAccess, true));
    __asm__ volatile("msr psp, %0" : : "r"(stack_top));
    control_set(control_get() | 2U);      /* SPSEL: thread mode uses PSP */
    g_sink = 0U;
    fault_clear();
    g_guard_arm = 1U;
    (void)recurse_deep(0U, &g_sink);
    g_guard_arm = 0U;
    /* Falling through means the overflow did not reach the guard. */
    control_set(control_get() & ~2U);     /* back to MSP */
    report_guard_result(false);
    run_summary();
}

[[noreturn]] void run_summary()
{
    harness::puts("MPU SUMMARY assert=");
    harness::put_u64(static_cast<uint64_t>(g_pass));
    harness::putc('/');
    harness::put_u64(static_cast<uint64_t>(g_pass + g_fail));
    harness::puts(" cap_enforced=");
    harness::put_u64(static_cast<uint64_t>(g_cap_enforced));
    harness::putc('/');
    harness::put_u64(static_cast<uint64_t>(g_cap_total));
    harness::puts(" required_enforced=");
    harness::put_u64(static_cast<uint64_t>(g_req_enforced));
    harness::putc('/');
    harness::put_u64(static_cast<uint64_t>(g_req_total));
    harness::newline();

    harness::puts("RESULT MPU ");
    harness::puts((0 == g_fail) ? "PASS " : "FAIL ");
    harness::put_u64(static_cast<uint64_t>(g_pass));
    harness::putc('/');
    harness::put_u64(static_cast<uint64_t>(g_pass + g_fail));
    harness::newline();
    for (;;) {
    }
}

extern "C" {
/* Reached from fault_common when the overflow really hit the guard. */
[[noreturn]] void guard_tail(void)
{
    mpu_region_off(6U);
    report_guard_result(0U != g_guard_resumed);
    run_summary();
}
}
