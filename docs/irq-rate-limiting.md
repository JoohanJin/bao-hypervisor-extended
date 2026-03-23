# IRQ Rate Limiting in Bao MCS

## 1. Overview
The IRQ Rate Limiting mechanism is designed to prevent "interrupt storms" from low-criticality Virtual Machines (VMs) or malfunctioning devices. In a Mixed-Criticality System (MCS), this ensures that safety-critical interrupts (`CRIT_HIGH`) are never starved of CPU time or bus bandwidth.

## 2. Token Bucket Algorithm
Bao implements rate limiting using a **Token Bucket** algorithm (defined in `src/core/inc/irq_rate_limit.h`).

### 2.1 Key Concepts
- **Tokens**: Represent the "budget" or "quota" for interrupt injections.
- **Bucket**: Each interrupt source (or VM) is assigned a bucket.
- **Refill Rate**: Tokens are replenished at a steady rate per second.
- **Burst Capacity**: The maximum number of tokens a bucket can hold (allows for short bursts of interrupts).

### 2.2 Mechanism Flow
1. **Refill**: On every interrupt attempt, the hypervisor updates the token count based on the elapsed time since the last refill.
2. **Consume**:
   - If tokens are available: The interrupt is allowed immediately (`IRQ_RL_ALLOW`).
   - If no tokens but buffer space is available: The interrupt is **deferred** (`IRQ_RL_DEFERRED`).
   - If no tokens and buffer is full: The interrupt is **dropped** (`IRQ_RL_DROPPED`).
3. **Drain**: Deferred interrupts are automatically "drained" and injected once tokens become available again.

## 3. Criticality-Based Configuration
The parameters for the token bucket are automatically tuned based on the criticality level assigned to the interrupt/VM:

| Parameter | CRIT_HIGH (Safety) | CRIT_LOW (Best-Effort) |
|-----------|--------------------|------------------------|
| **Max Tokens (Burst)** | 1000 | 100 |
| **Refill Rate** | 10,000 tokens/sec | 1,000 tokens/sec |
| **Max Deferred** | 32 IRQs | 8 IRQs |

*Note: These values are defaults and can be overridden via platform-specific defines.*

## 4. Implementation Status
- **Core Logic**: Fully implemented as inline functions in `src/core/inc/irq_rate_limit.h`.
- **Integration**: Currently in the integration phase. The infrastructure supports per-IRQ tracking via `struct vm_irq` in `vm.h`.

## 5. Usage Example (Internal API)
```c
struct irq_token_bucket bucket;

// Initialize for a high-criticality interrupt
irq_bucket_init(&bucket, CRIT_HIGH);

// Try to consume a token before injecting an IRQ
if (irq_bucket_consume(&bucket) == IRQ_RL_ALLOW) {
    vcpu_inject_irq(vcpu, irq_id);
}
```

## 6. Files Modified

| File | Change |
|---|---|
| `src/core/inc/irq_rate_limit.h` | Token bucket data structure, algorithm (init, refill, consume, drain_deferred), per-criticality defaults, `rdtime`-based cycle counter. |
| `src/arch/riscv/vplic.c` | Integration point — `vplic_inject()` calls `irq_bucket_consume()` before forwarding IRQs to guest vPLIC. Deferred IRQs stored in bitmap. |
| `src/arch/riscv/inc/arch/vplic.h` | Added `struct irq_token_bucket buckets[]` and `deferred` bitmap to `struct vplic`. |
| `src/core/inc/vm.h` | `struct vm_irq` with `.criticality` field for per-interrupt rate limiter parameterization. |
| `src/core/hypercall.c` | `HC_PLIC_INJECT` hypercall — direct rate limiter test path (bypasses vPLIC pending check). |
| `tests/test_irq_rate_limit.c` | 17 unit tests for token bucket logic (init, consume, refill, deferred, dropped, boundary conditions). |
