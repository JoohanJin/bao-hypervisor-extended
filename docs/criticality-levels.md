# Criticality Levels in Bao MCS

## 1. Definition
Criticality levels are used to classify Virtual Machines (VMs) and hardware resources (like interrupts) based on their safety importance.

- **CRIT_LOW (0)**: Best-effort or non-safety critical workloads (e.g., a Linux-based HMI or cloud connectivity stack).
- **CRIT_HIGH (1)**: Safety-critical workloads requiring strict deterministic behavior and high availability (e.g., a real-time control loop or safety logic).

## 2. Usage in the Hypervisor

### 2.1 VM Isolation
The criticality level is defined in the `vm_config` and stored in the `vm` structure. It serves as the foundation for the hypervisor's resource allocation and fault-handling policies.

### 2.2 IRQ Rate Limiting (Temporal Isolation)
Bao uses criticality levels to initialize the **Token Bucket** parameters for each interrupt:
- **High Criticality**: Large burst capacity and high refill rate.
- **Low Criticality**: Small burst capacity and aggressive throttling.

### 2.3 Per-Interrupt Criticality
The system supports assigning criticality levels to individual interrupts via the `struct vm_irq` structure. This allows a VM to manage a mix of safety-critical and non-critical interrupts within the same device context, ensuring that safety-critical interrupts receive priority and protection from interrupt storms.

### 2.4 Health Monitoring
The Health Monitor uses these levels to determine response thresholds. A failure in a `CRIT_HIGH` VM typically triggers an immediate system-level response, whereas a `CRIT_LOW` failure might only trigger a VM-local restart.

## 3. Configuration Example
```c
struct vm_config vm0 = {
    .criticality = CRIT_HIGH,
    .health = {
        .timeout_ms = 1000, // Strict 1s watchdog
    },
    /* ... */
};
```
