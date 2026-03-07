# Auto-Recovery for Failed Guest OSes — Design Plan

## 1. Overview

**Goal:** When the health monitor detects a guest VM as `VM_UNHEALTHY`
(i.e. it has missed `max_missed` consecutive heartbeats), the hypervisor
automatically restarts that VM without affecting other VMs.

**Branch:** `dev-health-monitor` (builds on commit `0cd1133`)

### What exists today (Phase 1 — detection only)

| Component | Status |
|---|---|
| Heartbeat hypercall (`HC_HEARTBEAT`) | Done |
| Timer-based liveness check | Done |
| State machine: HEALTHY → SUSPECT → UNHEALTHY | Done |
| Watchdog re-arm for hung guests | Done |
| Recovery action on UNHEALTHY | **Not done — this plan** |

---

## 2. Recovery Strategy

### 2.1 High-Level Flow

```
health_monitor_check() detects VM_UNHEALTHY
        │
        ▼
health_monitor_recover(vm)          ← NEW
        │
        ├─ 1. Pause all vCPUs belonging to this VM
        │     (current CPU's vCPU: direct stop)
        │     (remote vCPUs: send IPI VCPU_RESET message)
        │
        ├─ 2. Barrier: wait for all vCPUs to reach "stopped"
        │
        ├─ 3. Master CPU re-installs guest image
        │     (memcpy from config->image.load_addr → runtime region)
        │
        ├─ 4. Reset each vCPU's registers
        │     (vcpu_arch_reset(vcpu, vm->config->entry))
        │
        ├─ 5. Re-initialise health state → HEALTHY
        │
        └─ 6. Resume all vCPUs → guest reboots from entry point
```

### 2.2 Design Constraints

1. **Interrupt context** — `health_monitor_check()` runs inside
   `sbi_timer_irq_handler()`. Heavy work (memcpy of the image) should be
   deferred or performed with interrupts briefly re-enabled.

2. **Multi-vCPU coordination** — a VM can span multiple physical CPUs.
   All vCPUs must stop before memory is touched. Bao's existing
   `cpu_send_msg()` + IPI infrastructure handles this.

3. **Image preservation** — the guest binary is linked into the
   hypervisor image at a fixed `load_addr`. It is read-only and always
   available for re-copying. The `vm_install_image()` function already
   does this copy.

4. **No dynamic allocation** — Bao has no general-purpose heap at
   runtime. All recovery must use the existing `struct vm`, `struct
   vcpu[]`, page tables, and config pointers.

5. **Config availability** — `vm->config` is a `const` pointer set in
   `vm_master_init()`. It persists for the lifetime of the VM and
   provides `entry`, `image.load_addr`, `image.size`,
   `image.base_addr`.

---

## 3. Detailed Design

### 3.1 New Health States

Extend `enum vm_health_status`:

```c
enum vm_health_status {
    VM_HEALTHY       = 0,
    VM_SUSPECT       = 1,
    VM_UNHEALTHY     = 2,
    VM_NOT_MONITORED = 3,
    VM_RECOVERING    = 4,   /* NEW: recovery in progress */
};
```

Add to `struct vm_health`:

```c
struct vm_health {
    /* ... existing fields ... */
    uint32_t recovery_count;   /* NEW: total number of recoveries */
};
```

Add to `struct vm_health_config`:

```c
struct vm_health_config {
    /* ... existing fields ... */
    bool     auto_recover;     /* NEW: enable auto-restart on UNHEALTHY */
    uint32_t max_recoveries;   /* NEW: 0 = unlimited, N = stop after N */
};
```

### 3.2 New Files

| File | Purpose |
|---|---|
| `src/core/vm_recovery.c` | Recovery orchestration logic |
| `src/core/inc/vm_recovery.h` | Public API + IPI message event IDs |

### 3.3 Recovery Orchestration — `vm_recovery.c`

#### 3.3.1 Entry Point

Called from `health_monitor_check()` when status transitions to
`VM_UNHEALTHY`:

```c
void vm_recovery_start(struct vm *vm);
```

This function:
- Checks `vm->config->health.auto_recover` — if false, only log and
  return
- Checks `vm->health.recovery_count < max_recoveries` (unless 0 =
  unlimited)
- Sets `vm->health.status = VM_RECOVERING`
- Increments `vm->health.recovery_count`
- Sends `VM_RESET` IPI to all remote vCPUs via `vm_msg_broadcast()`
- Calls `vm_recovery_execute()` for the local vCPU

#### 3.3.2 IPI Message Handler

Register a new CPU message handler:

```c
enum VM_RECOVERY_EVENTS { VM_RESET };
void vm_recovery_msg_handler(uint32_t event, uint64_t data);
CPU_MSG_HANDLER(vm_recovery_msg_handler, VM_RECOVERY_MSG_ID);
```

On remote CPUs receiving `VM_RESET`:
1. Stop current vCPU execution (clear `active`, park in idle)
2. Wait on `vm->sync` barrier
3. Master performs image re-install
4. All vCPUs call `vcpu_arch_reset(vcpu, vm->config->entry)`
5. Barrier again
6. All vCPUs call `vcpu_run()` — guest reboots

#### 3.3.3 Image Re-Install

Make `vm_install_image()` in `vm.c` non-static (or add a public
wrapper):

```c
/* In vm.c — change from static to extern */
void vm_reinstall_image(struct vm *vm);
```

Implementation:
```c
void vm_reinstall_image(struct vm *vm)
{
    const struct vm_config *config = vm->config;

    /* Find the memory region containing the image */
    for (size_t i = 0; i < config->platform.region_num; i++) {
        struct vm_mem_region *reg = &config->platform.regions[i];
        if (range_in_range(config->image.base_addr, config->image.size,
                           reg->base, reg->size)) {
            vm_install_image(vm, reg);
            break;
        }
    }
}
```

This re-copies the pristine binary from `load_addr` into the VM's
runtime address space, overwriting any corrupted state.

#### 3.3.4 Full Recovery Sequence (Master CPU)

```c
static void vm_recovery_execute(struct vm *vm)
{
    printk("RECOVERY: VM %d starting recovery (#%d)\n",
           vm->id, vm->health.recovery_count);

    /* 1. Barrier — all vCPUs must be stopped */
    cpu_sync_barrier(&vm->sync);

    /* 2. Re-install guest image (master only) */
    vm_reinstall_image(vm);

    /* 3. Barrier — image is ready */
    cpu_sync_barrier(&vm->sync);

    /* 4. Reset our own vCPU */
    struct vcpu *vcpu = cpu()->vcpu;
    vcpu_arch_reset(vcpu, vm->config->entry);
    vcpu->arch.sbi_ctx.state = STARTED;

    /* 5. Re-init health state */
    health_monitor_init(vm, &vm->config->health);

    /* 6. Final barrier — all vCPUs reset */
    cpu_sync_barrier(&vm->sync);

    printk("RECOVERY: VM %d recovered successfully\n", vm->id);

    /* 7. Resume execution */
    vcpu_run(vcpu);
    /* does not return */
}
```

#### 3.3.5 Recovery Sequence (Remote CPUs)

```c
void vm_recovery_msg_handler(uint32_t event, uint64_t data)
{
    if (event != VM_RESET) return;

    struct vm *vm = cpu()->vcpu->vm;
    struct vcpu *vcpu = cpu()->vcpu;

    vcpu->active = false;

    /* 1. Barrier — signal we've stopped */
    cpu_sync_barrier(&vm->sync);

    /* 2. Barrier — wait for master to re-install image */
    cpu_sync_barrier(&vm->sync);

    /* 3. Reset our vCPU */
    vcpu_arch_reset(vcpu, vm->config->entry);
    vcpu->arch.sbi_ctx.state = vcpu->id == 0 ? STARTED : STOPPED;

    /* 4. Final barrier */
    cpu_sync_barrier(&vm->sync);

    /* 5. Resume */
    vcpu_run(vcpu);
}
```

### 3.4 Integration with health_monitor_check()

In `health_monitor.c`, when transitioning to `VM_UNHEALTHY`:

```c
if (vm->health.missed_count >= vm->health.max_missed) {
    vm->health.status = VM_UNHEALTHY;
    if (prev != VM_UNHEALTHY) {
        printk("HEALTH: VM %d UNHEALTHY (%d missed heartbeats)\n",
               vm->id, vm->health.missed_count);

        /* NEW: trigger auto-recovery */
        vm_recovery_start(vm);
    }
}
```

### 3.5 Integration with sbi_timer_irq_handler()

After recovery, the guest will re-program its own timer via
`sbi_set_timer()`. The watchdog re-arm logic already handles the case
where the guest hasn't programmed a timer yet, so no changes are needed
here.

### 3.6 Config Example

```c
/* demos/dual-baremetal/configs/qemu-riscv64-virt.c */
.health = {
    .enabled          = true,
    .heartbeat_period_ms = 1000,
    .timeout_ms       = 3000,
    .max_missed       = 3,
    .auto_recover     = true,    /* NEW */
    .max_recoveries   = 5,       /* NEW: stop after 5 recoveries */
},
```

---

## 4. Implementation Plan

### Phase 2a: Single-vCPU Recovery (Simpler)

If the VM has only one vCPU (which is the case for our QEMU baremetal
demo — each VM gets 1 master CPU + 1 secondary), we can simplify by
resetting only the requesting CPU's vCPU inline in the timer handler.

**Steps:**
1. Add `VM_RECOVERING` state and `recovery_count` field
2. Add `auto_recover` / `max_recoveries` config fields
3. Expose `vm_reinstall_image()` from `vm.c`
4. Implement `vm_recovery_start()` — reset current vCPU + reinstall
   image
5. Hook into `health_monitor_check()` on UNHEALTHY transition
6. Unit tests for recovery logic
7. QEMU integration test

### Phase 2b: Multi-vCPU Recovery (Full)

Extends Phase 2a with IPI-based coordination.

**Additional steps:**
8. Register `vm_recovery_msg_handler` with `CPU_MSG_HANDLER`
9. Implement barrier-based multi-vCPU stop/reset/resume
10. Test with multi-vCPU VM config

### Phase 2c: Recovery Policies (Future)

- Critical VMs (CRIT_HIGH): always recover, no limit
- Low-criticality VMs (CRIT_LOW): recover N times, then disable
- Recovery cooldown period (prevent rapid restart loops)
- IPC notification to a supervisor VM on recovery events

---

## 5. Files Modified/Created

| File | Change |
|---|---|
| `src/core/inc/health_monitor.h` | Add `VM_RECOVERING`, `recovery_count`, `auto_recover`, `max_recoveries` |
| `src/core/health_monitor.c` | Call `vm_recovery_start()` on UNHEALTHY transition |
| `src/core/inc/vm_recovery.h` | **NEW** — public API |
| `src/core/vm_recovery.c` | **NEW** — recovery orchestration |
| `src/core/vm.c` | Make `vm_install_image()` accessible (or add wrapper) |
| `src/core/objects.mk` | Add `vm_recovery.o` |
| `tests/test_vm_recovery.c` | **NEW** — unit tests for recovery logic |
| `tests/Makefile` | Add `test_vm_recovery` target |
| `demos/.../qemu-riscv64-virt.c` | Add `.auto_recover = true` |

---

## 6. Testing Strategy

### Unit Tests (host-side, mock-based)
- Recovery triggered only when `auto_recover = true`
- Recovery count increments
- Recovery refused when `max_recoveries` exceeded
- `VM_RECOVERING` state prevents re-entrant recovery
- Health state resets to `VM_HEALTHY` after recovery

### Integration Test (QEMU)
1. Build with auto-recovery enabled
2. Modify one guest to **stop sending heartbeats** after N timer IRQs
3. Observe: `HEALTH: VM X UNHEALTHY` → `RECOVERY: VM X starting` → 
   `RECOVERY: VM X recovered` → guest resumes printing timer IRQs
4. Verify the other VM is unaffected throughout

### Edge Cases
- Recovery during recovery (re-entrant — must be rejected)
- All vCPUs simultaneously unhealthy
- Recovery with `max_recoveries = 1` → first recovery succeeds, second
  is refused
- Guest image with `.inplace = true` vs `.inplace = false`

---

## 7. Key Risk: Timer Interrupt Context

`health_monitor_check()` runs in `sbi_timer_irq_handler()` which is
interrupt context. The recovery flow involves:
- `memcpy` for image re-install (potentially large)
- `cpu_sync_barrier()` which spins

**Mitigation options:**
1. **Deferred execution** — set a flag in the timer handler, perform
   recovery in a separate context (e.g. after returning from interrupt,
   check flag before `vcpu_arch_run()`). This is preferred.
2. **Direct execution** — perform inline. Works because Bao is a
   type-1 hypervisor with minimal interrupt nesting. The timer handler
   already does non-trivial work (CSR manipulation, printk).

**Chosen approach:** Option 1 — set `VM_RECOVERING` flag in timer
handler, check it in the return-to-guest path. This keeps the timer
handler lightweight and avoids blocking other interrupts during a
potentially slow image copy.

### Deferred Recovery Hook Point

In `sbi_timer_irq_handler()` → exits → before `vcpu_arch_run()` resumes
guest, check:

```c
/* In the exception return path or vcpu_run() */
if (cpu()->vcpu->vm->health.status == VM_RECOVERING) {
    vm_recovery_execute(cpu()->vcpu->vm);
    /* does not return — re-enters vcpu_run() after reset */
}
```

This can be placed in `vcpu_arch_run()` or in the exception return path
in `exceptions.S` / `sync_exceptions.c`.

---

## 8. Known Limitations for Mixed-Criticality Systems

### 8.1 Blocking Recovery Window

The recovery process is **not instantaneous**. Between the moment
`VM_UNHEALTHY` is detected and the moment the guest resumes execution,
the following blocking operations occur:

1. **Image re-installation** — `memcpy` copies the entire guest binary
   from `load_addr` into the VM's runtime memory. For a non-trivial
   guest image this can take **hundreds of microseconds to milliseconds**
   depending on image size and memory bandwidth.

2. **Multi-vCPU barrier synchronisation** — all vCPUs belonging to the
   failed VM must reach a barrier before the image copy starts, and
   again before they resume. If one vCPU is slow to respond to the IPI,
   **all vCPUs spin-wait**, including any that share physical resources
   (cache, memory bus) with other VMs.

3. **Interrupt masking** — during the deferred recovery path, timer
   interrupts for the recovering VM's CPUs are effectively blocked. If
   the recovery is slow, watchdog timers on *other* VMs sharing the same
   physical CPU (not applicable in the current partitioned config, but
   possible in time-sliced / shared-CPU configs) could see timing jitter.

**Impact on MCS guarantees:**

In a true mixed-criticality deployment, a CRIT_HIGH VM must maintain
bounded worst-case execution time (WCET) and interrupt latency at all
times. The blocking recovery of a co-resident CRIT_LOW VM can violate
these bounds because:

- The `memcpy` and barrier spins consume shared bus bandwidth.
- If recovery runs on a CPU that also hosts a CRIT_HIGH vCPU (shared-CPU
  scenario), the CRIT_HIGH guest is paused for the duration.
- There is **no preemption** of the recovery path — once started, it
  runs to completion.

**This is a fundamental limitation of the current design.** A
production MCS hypervisor would require:

- **Hardware-assisted memory restore** (e.g. DMA-based image copy that
  does not stall the CPU, or checkpoint/restore at the MMU level).
- **Non-blocking recovery** with bounded time that can be incorporated
  into the WCET analysis of co-resident VMs.
- **Temporal isolation** guarantees (e.g. memory bandwidth regulation,
  cache partitioning via page colouring — Bao supports colouring but
  not bandwidth regulation).
- **Formal verification** that recovery cannot cause deadline misses
  on CRIT_HIGH partitions.

For this prototype / FYP scope, the recovery is implemented as a
**best-effort mechanism** that demonstrates the concept. The blocking
window is acceptable for the QEMU baremetal demo where:
- Each VM is statically pinned to dedicated physical CPUs (no sharing).
- Image sizes are small (~4–8 KB baremetal binaries).
- There are no hard real-time deadlines to violate.

### 8.2 No State Preservation

Recovery performs a **full cold restart** — all guest RAM is overwritten
with the original image, and vCPU registers are zeroed. Any in-flight
computation, application state, or IPC message buffers are lost. This is
equivalent to a power cycle, not a graceful restart.

For stateful guests (e.g. Linux), this means:
- File system corruption if writes were in progress.
- Loss of network connections and application context.
- No guarantee that the guest will reach the same operational state.

A checkpoint/restore mechanism would be needed for stateful recovery,
which is out of scope for this prototype.

### 8.3 No Root-Cause Diagnosis

The hypervisor detects *that* a VM failed (missed heartbeats) but not
*why*. The guest could be:
- Stuck in an infinite loop
- Crashed with an unhandled exception
- Deadlocked on a spinlock
- Starved of CPU time by a misconfigured scheduler

Recovery restarts the guest regardless. If the root cause is
environmental (e.g. hardware fault, memory corruption), the guest will
likely fail again immediately, consuming recovery attempts until
`max_recoveries` is exhausted.

### 8.4 Recovery Count as a Soft Bound

The `max_recoveries` limit prevents infinite restart loops but is not
a safety mechanism — it is a heuristic. A proper MCS system would
require formal analysis of recovery frequency and its impact on system
availability.

---

## 9. Summary

The auto-recovery feature extends the existing health monitor with a
lightweight restart mechanism. It reuses Bao's existing primitives:
- `vcpu_arch_reset()` — already proven in SBI HSM HART_START
- `vm_install_image()` — already used during boot
- `cpu_send_msg()` + IPI — already used for SBI IPI forwarding
- `cpu_sync_barrier()` — already used during `vm_init()`

No new memory allocation, no new page table setup, no changes to the
guest binary format. The guest simply reboots from its original entry
point with a fresh image, as if power-cycled.

**However**, the blocking nature of image re-installation and multi-vCPU
synchronisation makes this approach unsuitable for production MCS
deployments where bounded recovery latency must be guaranteed. See
Section 8 for a full discussion of limitations.
