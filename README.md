# Bao Hypervisor – MCS Extensions Fork

This fork of the Bao hypervisor implements **Mixed-Criticality Systems (MCS)** extensions for a Final Year Project (FYP) research program. The goal is to add criticality-aware isolation, interrupt determinism, and fault containment mechanisms aligned with safety standards (IEC 61508, ISO 26262).

## Project Overview

**Objective:** Extend Bao's static partitioning hypervisor with MCS features to achieve **Freedom From Interference (FFI)** between VMs of different criticality levels.

**Target Platform:** RISC-V 64-bit (QEMU virt machine, with future hardware targets)

**Development Environment:** macOS Apple Silicon → RISC-V cross-compilation

## Why Bao for MCS
- **Minimal static partitioning hypervisor** providing 1:1 vCPU:pCPU mapping, pass-through devices, and two-stage memory translation
- **Strong isolation & real-time focus** suited for mixed-criticality systems in automotive, avionics, and industrial deployments
- **Small TCB (~10K SLOC)**: no dependence on privileged general-purpose operating systems
- **Deterministic behavior**: no dynamic resource allocation at runtime

## MCS Extension Roadmap

### Phase 1: Criticality Levels ✅ (Implemented)
- Added `enum vm_criticality { CRIT_LOW, CRIT_HIGH }` to VM configuration
- Each VM is assigned a criticality level at boot time
- **IRQ Rate Limiting**: Implemented a Token Bucket algorithm that throttles interrupts based on criticality to prevent interrupt storms and ensure temporal isolation.
- Foundation for criticality-aware resource management.

See [docs/criticality-levels.md](docs/criticality-levels.md) for detailed design.

### Phase 2: IRQ Budget & Rate Limiting (Planned)
- Per-VM interrupt budgets with configurable limits
- Rate limiting to prevent low-criticality VMs from starving high-criticality VMs
- Interrupt accounting and throttling mechanisms

**Planned additions:**
```c
struct vm_config {
    // ...
    uint32_t irq_budget;        // Max IRQs per time window
    uint32_t irq_window_ms;     // Time window in milliseconds
};
```

### Phase 3: IOMMU Integration (Planned)
- Per-VM DMA isolation using RISC-V IOMMU
- Device-to-VM binding with address space protection
- Prevent DMA attacks from compromising high-criticality VMs

### Phase 4: Health Monitor (Planned)
- Monitor VM health and detect failures
- Optional automatic restart of failed low-criticality VMs
- Preserve high-criticality VM operation during recovery

## Safety Standards Alignment

| Requirement | IEC 61508 | ISO 26262 | Bao MCS Implementation |
|-------------|-----------|-----------|------------------------|
| Spatial isolation | SIL 3/4 | ASIL D | Two-stage MMU, IOMMU |
| Temporal isolation | SIL 3/4 | ASIL D | IRQ budgets, rate limiting |
| Criticality separation | Table A.2 | Part 9 | Criticality levels per VM |
| Fault containment | Clause 7.4 | Part 6 | Health Monitor, VM restart |

## Repository Structure

```
src/
├── core/
│   ├── inc/
│   │   ├── vm.h          # VM struct with criticality field
│   │   └── config.h      # VM config with criticality
│   └── vm.c              # VM initialization
├── arch/riscv/
│   ├── iommu.c           # IOMMU driver (stub)
│   └── ...
└── ...
```

## Current Branch: `dev-criticality_level`

This branch contains the Phase 1 implementation of criticality levels.

## Building & Testing

### Quick Start (using bao-demos)
The easiest way to build and test is using the companion `bao-demos` repository:

```bash
cd /path/to/bao-demos
./build.sh      # Build OpenSBI, Bao, and baremetal guest
./deploy.sh     # Launch QEMU emulation
./cleanup.sh    # Clean all build artifacts
```

### Manual Build
```bash
export CROSS_COMPILE=riscv64-unknown-elf-
make PLATFORM=qemu-riscv64-virt CONFIG=baremetal
```

### VM Configuration with Criticality
```c
// In your config.c
struct vm_config vm0 = {
    .entry = 0x80200000,
    .criticality = CRIT_HIGH,  // High-criticality VM
    .platform = { ... },
    // ...
};
```

## Testing the MCS Extensions

### Test 1: Criticality Level Assignment
Verify VMs are assigned correct criticality levels at boot.

### Test 2: IRQ Rate Limiting (Phase 2)
- Configure low-crit VM with IRQ budget
- Generate interrupt storm
- Verify high-crit VM latency unaffected

### Test 3: Fault Isolation (Phase 4)
- Inject fault in low-crit VM
- Verify high-crit VM continues operation
- Observe Health Monitor recovery

## Validation & Measurement Plan
- **IOMMU isolation tests**: negative DMA scenarios, per-device domain verification
- **Latency harness**: timestamp hooks, WCRT analysis with histogram capture
- **Determinism tooling**: bandwidth throttling experiments, WCET comparison

## References
1. José Martins et al., "Bao: A Lightweight Static Partitioning Hypervisor for Modern Multi-Core Embedded Systems," NG-RES 2020.
2. José Martins and Sandro Pinto, "Bao: a modern lightweight embedded hypervisor," Embedded World 2020.
3. José Martins and Sandro Pinto, "Static Partitioning Virtualization on RISC-V," RISC-V Summit 2020.
4. Bruno Sá et al., "A First Look at RISC-V Virtualization from an Embedded Systems Perspective," IEEE TC 2021.
5. José Martins and Sandro Pinto, "Shedding Light on Static Partitioning Hypervisors for Arm-based Mixed-Criticality Systems," RTAS 2023.

## License
Bao is distributed under GPLv2 (see `LICENSE`). This fork's extensions follow the same license.

## Author
FYP Research Project – December 2025
