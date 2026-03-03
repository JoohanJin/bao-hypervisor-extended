# VM Health Monitor — Design Plan

**Branch:** `dev-health-monitor`
**Status:** Planning only — no code changes yet

---

## 1. Problem Statement

In a Mixed-Criticality System, the hypervisor partitions hardware across
guest VMs but has **no visibility into whether a VM is actually alive and
functioning**. A CRIT_HIGH safety-critical VM could hang, deadlock, or enter
an infinite loop, and the hypervisor would never know.

We need a **heartbeat-based health monitoring** facility where:
- Guest VMs periodically signal the hypervisor that they are alive
- The hypervisor detects missed heartbeats and flags a VM as unhealthy
- (Future) The hypervisor can trigger recovery actions (restart, failover)

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│                   Bao Hypervisor                    │
│                                                     │
│  ┌──────────────┐         ┌──────────────────────┐  │
│  │ Health       │  timer  │ Per-VM state:         │  │
│  │ Monitor      │◄───────►│  last_heartbeat       │  │
│  │ (hyp timer)  │         │  health_status        │  │
│  └──────┬───────┘         │  missed_count         │  │
│         │ check           │  timeout / period     │  │
│         │                 └──────────────────────┘  │
│         │                        ▲                   │
│         │                        │ HC_HEARTBEAT      │
│─────────┼────────────────────────┼───────────────────│
│  Guest VM0 (CRIT_HIGH)    Guest VM1 (CRIT_LOW)      │
│                                                      │
│  heartbeat_send()         heartbeat_send()           │
│  (periodic ecall)         (periodic ecall)           │
└──────────────────────────────────────────────────────┘
```

**Model:** Server (hypervisor) — Client (guest VMs), using hypercalls.

---

## 3. What Does "Healthy" Mean?

A VM is considered **healthy** if it can execute its heartbeat hypercall
within the configured deadline. This captures:

| Failure mode             | Detected? | Reasoning                              |
|--------------------------|-----------|----------------------------------------|
| Guest hang / deadlock    | ✅ Yes    | Heartbeat hypercall stops firing       |
| Infinite loop (no IRQ)   | ✅ Yes    | Timer IRQ masked → no heartbeat call   |
| Guest crash / exception  | ✅ Yes    | Execution stops → no heartbeat         |
| Guest slow / overloaded  | ⚠️ Partial| Long deadline hides latency issues     |
| Data corruption          | ❌ No     | Guest may still send heartbeats        |
| Logical errors           | ❌ No     | Not observable via liveness alone      |

**Health states:**

```c
enum vm_health_status {
    VM_HEALTHY,         /* Heartbeats arriving on time */
    VM_SUSPECT,         /* 1+ missed heartbeats, within grace period */
    VM_UNHEALTHY,       /* Exceeded max missed heartbeats */
    VM_NOT_MONITORED,   /* Heartbeat not configured for this VM */
};
```

---

## 4. Detailed Design

### 4.1 New Hypercall: `HC_HEARTBEAT`

```c
/* In hypercall.h */
enum { HC_INVAL, HC_IPC, HC_HEARTBEAT };
```

Guest VMs call this via the existing SBI ecall interface:

```c
/* Guest-side code */
void heartbeat_send(void) {
    sbi_ecall(SBI_EXTID_BAO, HC_HEARTBEAT, 0, 0, 0, 0, 0, 0);
}
```

**Hypervisor handler** (in `hypercall.c`):

```c
case HC_HEARTBEAT:
    health_monitor_heartbeat(cpu()->vcpu->vm);
    break;
```

`health_monitor_heartbeat()` records the current time and resets the missed
counter:

```c
void health_monitor_heartbeat(struct vm *vm) {
    vm->health.last_heartbeat = rdtime();
    vm->health.missed_count = 0;
    vm->health.status = VM_HEALTHY;
}
```

### 4.2 Hypervisor-Side Timer (Watchdog)

Bao currently forwards S-mode timer interrupts to the guest. For the health
monitor, we need the hypervisor to keep its own periodic timer on **one
designated hart** (e.g., VM0's master CPU or a dedicated monitor hart).

**Approach — Steal-and-forward:**
1. On the monitor hart's timer interrupt, the hypervisor:
   - Runs the health check (compare `rdtime()` vs `last_heartbeat`)
   - Reprograms `sbi_set_timer(now + check_interval)`
   - Then forwards the timer interrupt to the guest as before
2. This adds minimal latency (~10s of cycles) to guest timer delivery.

**Alternative — Dedicated hart:** If a spare pCPU exists, dedicate it to
monitoring. Simpler but wastes a core.

**Check interval:** Configurable per-VM. Reasonable defaults:
- CRIT_HIGH: check every 100ms, timeout at 500ms (5 missed = UNHEALTHY)
- CRIT_LOW: check every 500ms, timeout at 2000ms (4 missed = UNHEALTHY)

### 4.3 Per-VM Health State

Add to `struct vm`:

```c
struct vm_health {
    enum vm_health_status status;
    uint64_t last_heartbeat;      /* rdtime() value at last heartbeat */
    uint32_t missed_count;        /* consecutive missed checks */
    uint32_t max_missed;          /* threshold before UNHEALTHY */
    uint64_t check_interval;      /* timer ticks between checks */
    uint64_t heartbeat_timeout;   /* ticks before a check is "missed" */
};
```

Add to `struct vm_config`:

```c
struct vm_health_config {
    bool     enabled;             /* false = VM_NOT_MONITORED */
    uint64_t heartbeat_period_ms; /* expected guest heartbeat interval */
    uint64_t timeout_ms;          /* deadline before declaring missed */
    uint32_t max_missed;          /* missed checks before UNHEALTHY */
};
```

### 4.4 Health Check Logic (Pseudocode)

```
On timer interrupt (monitor hart):
    for each vm in config.vmlist:
        if vm.health.status == VM_NOT_MONITORED:
            continue

        elapsed = rdtime() - vm.health.last_heartbeat

        if elapsed > vm.health.heartbeat_timeout:
            vm.health.missed_count++

            if vm.health.missed_count == 1:
                vm.health.status = VM_SUSPECT
                LOG("VM %d: heartbeat missed (1/%d)", vm_id, max_missed)

            if vm.health.missed_count >= vm.health.max_missed:
                vm.health.status = VM_UNHEALTHY
                LOG("VM %d: UNHEALTHY — %d missed heartbeats", vm_id, missed)
                health_monitor_on_failure(vm)  /* future: recovery */
```

### 4.5 Cross-CPU Propagation

Since Bao is a partitioning hypervisor — each pCPU runs one vCPU — the
health check must run on the monitor hart but may need to notify other
harts. Use the existing `cpu_send_msg()` + `CPU_MSG_HANDLER` mechanism:

```c
CPU_MSG_HANDLER(health_msg_handler, HEALTH_MONITOR_IPI_ID);

/* Events */
enum {
    HEALTH_EVENT_STATUS_CHANGE,  /* notify other harts of status change */
    HEALTH_EVENT_QUERY,          /* request current status from a hart */
};
```

### 4.6 Guest-Side Integration

Each guest VM needs a periodic heartbeat sender. Two options:

**Option A — Timer-driven (recommended):**
The guest's timer interrupt handler calls `heartbeat_send()` alongside its
normal work. If the timer ISR can fire, the guest is alive.

```c
void timer_handler(unsigned id) {
    heartbeat_send();        /* <-- add this */
    timer_rearm();
    /* ... normal timer work ... */
}
```

**Option B — Main-loop driven:**
For guests with a main loop (non-interrupt-driven), call from the loop:

```c
while (1) {
    heartbeat_send();
    do_work();
}
```

### 4.7 Future: Recovery Actions

Not in scope for initial implementation, but the design should accommodate:

| Recovery action         | Mechanism                                       |
|-------------------------|-------------------------------------------------|
| Log & alert             | `printk()` on hypervisor console                |
| Notify other VMs        | IPC doorbell to CRIT_HIGH VM                    |
| Pause faulty VM         | Set `vcpu->arch.sbi_ctx.state = STOPPED`        |
| Restart faulty VM       | Needs new `vm_reset()` — reload image, reinit   |
| Reallocate resources    | Redistribute IRQs/memory to surviving VMs       |

---

## 5. Files to Create / Modify

| File                              | Action  | Description                              |
|-----------------------------------|---------|------------------------------------------|
| `src/core/inc/health_monitor.h`   | CREATE  | Health state structs, API declarations   |
| `src/core/health_monitor.c`       | CREATE  | Check logic, heartbeat handler, init     |
| `src/core/inc/hypercall.h`        | MODIFY  | Add `HC_HEARTBEAT = 2`                   |
| `src/core/hypercall.c`            | MODIFY  | Dispatch `HC_HEARTBEAT`                  |
| `src/core/inc/vm.h`               | MODIFY  | Add `struct vm_health` to `struct vm`    |
| `src/core/inc/config.h`           | MODIFY  | Add `struct vm_health_config`            |
| `src/arch/riscv/sbi.c`            | MODIFY  | Insert health check in timer IRQ path    |
| `src/core/vm.c`                   | MODIFY  | Init health state in `vm_init()`         |
| `tests/test_health_monitor.c`     | CREATE  | Unit tests (host-side, mocked timer)     |
| Guest `main.c` files              | MODIFY  | Add `heartbeat_send()` to timer handler  |
| Demo configs                      | MODIFY  | Add `health_config` fields               |

---

## 6. Implementation Phases

### Phase 1: Minimal Heartbeat (MVP)
- Add `HC_HEARTBEAT` hypercall
- Add `struct vm_health` to `struct vm`
- Hypervisor-side: record `last_heartbeat` timestamp
- Hypervisor-side: periodic check in timer IRQ path
- Console logging on status changes
- Unit tests for health check logic

### Phase 2: Criticality-Aware Monitoring
- Different timeout/threshold parameters per criticality level
- CRIT_HIGH VMs get stricter deadlines
- CRIT_LOW failures don't affect CRIT_HIGH operation

### Phase 3: Recovery Actions
- `vm_reset()` implementation
- IPC notification to surviving VMs
- Configurable recovery policy per VM

---

## 7. Open Questions

1. **Timer sharing:** How to share the S-timer between health monitor and
   guest timer virtualization without adding jitter to guest timing?

2. **Monitor hart selection:** Should the monitor run on a dedicated hart,
   or piggyback on VM0's master CPU? Dedicated is cleaner but wastes a core
   on small platforms.

3. **Heartbeat frequency:** How often should guests call the hypercall?
   Too often = overhead. Too rarely = slow detection. Need empirical tuning.

4. **False positives:** A long IRQ handler in a CRIT_HIGH guest could
   delay the heartbeat without the VM being unhealthy. Should the timeout
   account for worst-case interrupt latency?

5. **Minimal guest changes:** Can we make the heartbeat transparent by
   hooking into an existing mechanism (e.g., guest timer virtualization trap)
   rather than requiring explicit guest code changes?
