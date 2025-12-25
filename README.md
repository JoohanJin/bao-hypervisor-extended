# Bao Hypervisor – Program Overview

This private fork of the Bao hypervisor tracks a research and engineering program focused on reinforcing isolation, interrupt latency, and memory determinism on RISC-V platforms. The groundwork remains Bao's lightweight static-partitioning hypervisor core, while this repository layers planning for new architectural features, validation harnesses, and safety evidence.

## Why Bao
- **Minimal static partitioning hypervisor** providing 1:1 vCPU:pCPU mapping, pass-through devices, and two-stage memory translation.
- **Strong isolation & real-time focus** suited for mixed-criticality systems in automotive, avionics, and industrial deployments.
- **Small TCB**: no dependence on privileged general-purpose operating systems.

Supported platforms mirror upstream Bao (Armv8 A/R, Armv7, RISC-V RV32/64, Tricore, Renesas), but current work is centered on RISC-V `virt` targets.

## Current Priorities
The roadmap is captured in `docs/roadmap/priorities-and-milestones.md` and centers on:
1. **DMA isolation via IOMMU** – introduce per-VM domains and device attach workflows.
2. **Bounded-latency interrupt delivery** – leverage RISC-V Advanced Interrupt Architecture (AIA) with MSI routing direct to VS-mode and latency instrumentation.
3. **Memory determinism hardening** – add page-coloring, bandwidth controls, and disaster-control strategies for faulty memory or rogue interrupt sources.
4. **RSA-like Memory Fault Manager** (not yet decided) - add the SIF to manage the hardware memory fault.

## Repository Additions
Recent scaffolding organizes implementation and evidence work without altering the upstream source layout.

- `src/arch/riscv/iommu/` – placeholder for the RISC-V IOMMU driver stack.
- `src/arch/riscv/aia/` – staging for AIA support and interrupt-latency hooks.
- `configs/extensions/` – reusable configuration fragments to exercise new features.
- `scripts/qemu/` – harness scripts for QEMU `virt` experiments and measurement automation.
- `docs/roadmap/` – priorities and milestone tracking.
- `docs/deliverables/` – top-level hub for:
  - **Bao enhancements** (`docs/deliverables/bao/`)
  - **Safety artifacts** (`docs/deliverables/safety-artifacts/` with hazard, FFI, WCRT, DMA negative-test, and config checklist placeholders)
  - **Write-up materials** (`docs/deliverables/write-up/` for IRQ/DMA diagrams, timing histograms, RT budget tables, and supporting pasted content)

Each area currently hosts README placeholders to guide future content drops (no implementation code is committed yet).

## Building the Hypervisor
Bao remains a make-based project. Typical flow for a RISC-V QEMU bring-up:

```bash
# Example toolchain prefix (update to your environment)
export CROSS_COMPILE=riscv64-unknown-elf-

# Build Bao with two guest VMs using the example config
make PLATFORM=qemu-riscv64-virt CONFIG=example
```

Key points:
- `PLATFORM` must refer to a directory under `src/platform/`.
- `CONFIG` points to a C source under `configs/` (or a subdirectory with `config.c`).
- Outputs land in `build/<platform>/<config>/` and `bin/<platform>/<config>/` as ELF and binary images.
- `CONFIG_REPO` can redirect configuration sources outside the repo when integrating product assets.

Consult upstream Bao documentation for board-specific instructions: https://bao-project.readthedocs.io/.

## Validation & Measurement Plan
The deliverables roadmap introduces:
- **IOMMU isolation tests**: negative DMA scenarios, per-device domain verification.
- **Latency harness**: direct MSI routing, timestamp hooks, WCRT analysis with histogram capture.
- **Determinism tooling**: page-coloring utilities, bandwidth throttling experiments, and WCET comparison reporting.

Artifacts from these activities will populate the `docs/deliverables/` tree as they are produced.

## Safety Case Slice
Safety objectives align with IEC 61508 / ISO 26262 and ARINC partitioning claims. The evidence chain will link hazard IDs to goals, requirements, controls, and observations. Placeholders are ready for:
- Hazard log
- Freedom-from-interference (FFI) argument
- WCRT plots
- Configuration lock-down checklist

## References
1. José Martins et al., "Bao: A Lightweight Static Partitioning Hypervisor for Modern Multi-Core Embedded Systems," NG-RES 2020.
2. José Martins and Sandro Pinto, "Bao: a modern lightweight embedded hypervisor," Embedded World 2020.
3. José Martins and Sandro Pinto, "Static Partitioning Virtualization on RISC-V," RISC-V Summit 2020.
4. Bruno Sá et al., "A First Look at RISC-V Virtualization from an Embedded Systems Perspective," IEEE Transactions on Computers, 2021.
5. Samuel Pereira et al., "Bao-Enclave: Virtualization-based Enclaves for Arm," 2022.
6. José Martins and Sandro Pinto, "Shedding Light on Static Partitioning Hypervisors for Arm-based Mixed-Criticality Systems," RTAS 2023.
7. José Martins and Sandro Pinto, "Porting of a Static Partitioning Hypervisor to Arm's Cortex-R52," EOSS 2023.
8. David Cerdeira and José Martins, "Hello 'Bao' World" Tutorial, Bao Half-Day Workshop 2023.
9. João Peixoto et al., "BiRtIO: VirtIO for Real-Time Network Interface Sharing on the Bao Hypervisor," IEEE Access, 2024.
10. Hidemasa Kawasaki and Soramichi Akiyama, "Running Bao Hypervisor on gem5," gem5 blog, 2024.

## License
Bao is distributed under the Apache 2.0 license (see `LICENSE`).

## Change Log
All repository setup actions are logged in date-stamped files such as `2025-11-06_v1.log`, capturing timestamps, operation types, and descriptions for traceability.
