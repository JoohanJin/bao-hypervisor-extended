# RISC-V IOMMU Support for DMA Filtration — Design Plan

**Branch:** `dev-iommu_support`
**Status:** Planning

---

## 1. Problem Statement

In a partitioning hypervisor like Bao, the CPU-side memory isolation is
enforced via second-stage page tables (`hgatp`). However, DMA-capable
devices can bypass CPU address translation entirely — a device programmed
by a compromised or buggy guest VM can read/write **any** physical memory,
breaking the isolation boundary between VMs.

**Without an IOMMU:**

```
 Guest VM0 (CRIT_HIGH)         Guest VM1 (CRIT_LOW)
       │                              │
  programs device A              programs device B
       │                              │
       ▼                              ▼
  ┌─────────┐                    ┌─────────┐
  │ Device A │                   │ Device B │
  └────┬─────┘                   └────┬─────┘
       │  DMA (no translation)        │  DMA (no translation)
       ▼                              ▼
  ╔══════════════════════════════════════════╗
  ║        Physical Memory (DRAM)            ║
  ║   VM0 region  │  VM1 region  │ HYP      ║
  ╚══════════════════════════════════════════╝
       ← Device B can reach VM0's memory! →
```

This is a critical MCS violation: a low-criticality guest can corrupt
high-criticality guest memory through DMA.

**With an IOMMU:**

```
 Guest VM0 (CRIT_HIGH)         Guest VM1 (CRIT_LOW)
       │                              │
  programs device A              programs device B
       │                              │
       ▼                              ▼
  ┌─────────┐                    ┌─────────┐
  │ Device A │                   │ Device B │
  └────┬─────┘                   └────┬─────┘
       │  DMA                         │  DMA
       ▼                              ▼
  ┌──────────────────────────────────────────┐
  │           RISC-V IOMMU                   │
  │  Device A → VM0 page tables (G-stage)    │
  │  Device B → VM1 page tables (G-stage)    │
  │  Unmapped devices → FAULT (blocked)      │
  └──────────────────────────────────────────┘
       │                              │
       ▼ (translated)                 ▼ (translated)
  ╔══════════════════════════════════════════╗
  ║        Physical Memory (DRAM)            ║
  ║   VM0 region  │  VM1 region  │ HYP      ║
  ╚══════════════════════════════════════════╝
       ← Device B CANNOT reach VM0's memory →
```

### 1.1 MCS Relevance

| Property              | Without IOMMU       | With IOMMU              |
|-----------------------|---------------------|-------------------------|
| DMA isolation         | None                | Per-VM page tables      |
| Fault containment     | DMA can corrupt any | DMA faults at boundary  |
| Device assignment     | Trust-based only    | Hardware-enforced       |
| Criticality boundary  | Violated by DMA     | Preserved end-to-end    |

---

## 2. Hardware: RISC-V IOMMU on QEMU virt

### 2.1 QEMU Configuration

QEMU 10.1.3 supports the RISC-V IOMMU specification (ratified) on the
`virt` machine via the `iommu-sys` option:

```bash
qemu-system-riscv64 -M virt,iommu-sys=on ...
```

This instantiates a platform IOMMU device in the device tree:

```dts
iommu@3010000 {
    compatible = "riscv,iommu";
    reg = <0x00 0x3010000 0x00 0x1000>;   /* MMIO base & size */
    #iommu-cells = <0x01>;
    interrupts = <0x24 0x25 0x26 0x27>;   /* PLIC IRQs 36-39 */
    interrupt-parent = <&plic>;
};

pci@30000000 {
    iommu-map = <0x00 &iommu 0x00 0xffff>;
    /* All PCI BDF device_ids map 1:1 through IOMMU */
};
```

**Key hardware parameters:**
- **MMIO base address:** `0x03010000`
- **Register space size:** `0x1000` (4 KiB)
- **Interrupt lines:** 4 IRQs on PLIC (36=CQ, 37=FQ, 38=PM, 39=PQ)
- **Device IDs:** PCI BDF mapped 1:1 (0x0000–0xFFFF)
- **Default mode:** BARE (pass-through) or OFF

### 2.2 RISC-V IOMMU Register Map

All registers are MMIO at `base + offset`. Minimum access 4 bytes, aligned.

| Offset   | Size | Register | Description                       |
|----------|------|----------|-----------------------------------|
| `0x0000` | 64b  | CAP      | Capabilities (read-only)          |
| `0x0008` | 32b  | FCTL     | Feature control                   |
| `0x0010` | 64b  | DDTP     | Device Directory Table Pointer    |
| `0x0018` | 64b  | CQB      | Command Queue Base                |
| `0x0020` | 32b  | CQH      | Command Queue Head                |
| `0x0024` | 32b  | CQT      | Command Queue Tail                |
| `0x0028` | 64b  | FQB      | Fault Queue Base                  |
| `0x0030` | 32b  | FQH      | Fault Queue Head                  |
| `0x0034` | 32b  | FQT      | Fault Queue Tail                  |
| `0x0038` | 64b  | PQB      | Page Request Queue Base           |
| `0x0040` | 32b  | PQH      | Page Request Queue Head           |
| `0x0044` | 32b  | PQT      | Page Request Queue Tail           |
| `0x0048` | 32b  | CQCSR    | Command Queue Control/Status      |
| `0x004C` | 32b  | FQCSR    | Fault Queue Control/Status        |
| `0x0050` | 32b  | PQCSR    | Page Request Queue Control/Status |
| `0x0054` | 32b  | IPSR     | Interrupt Pending Status (W1C)    |
| `0x02F8` | 64b  | ICVEC    | Interrupt Cause → Vector mapping  |

### 2.3 Key Register Details

#### DDTP — Device Directory Table Pointer (`0x0010`)

```
 63                    10  9  5  4   3       0
┌────────────────────────┬────┬───┬──────────┐
│         PPN            │rsv │BSY│   MODE   │
└────────────────────────┴────┴───┴──────────┘
```

| MODE | Value | Description                                    |
|------|-------|------------------------------------------------|
| OFF  | 0     | IOMMU off — all DMA transactions faulted       |
| BARE | 1     | Pass-through — no address translation           |
| 1LVL | 2    | 1-level DDT — up to 64 devices (6-bit dev_id)  |
| 2LVL | 3    | 2-level DDT — up to 32K devices (15-bit dev_id)|
| 3LVL | 4    | 3-level DDT — up to 16M devices (24-bit dev_id)|

**For our use case:** 1LVL mode is sufficient (we only have a few
virtual devices). PPN points to a page containing Device Context entries.

#### FCTL — Feature Control (`0x0008`)

```
Bit 0: BE   — Big-endian (0 = little-endian)
Bit 1: WSI  — Wired Signaled Interrupts enable (set to 1 for PLIC)
Bit 2: GXL  — G-stage XLEN (0 = 64-bit)
```

#### CQCSR — Command Queue Control/Status (`0x0048`)

```
Bit 0:  CQEN  — Enable command queue
Bit 16: CQON  — Command queue active (read-only)
Bit 17: BUSY  — Busy (read-only)
```

#### FQCSR — Fault Queue Control/Status (`0x004C`)

```
Bit 0:  FQEN  — Enable fault queue
Bit 16: FQON  — Fault queue active (read-only)
```

#### IPSR — Interrupt Pending Status (`0x0054`)

```
Bit 0: CIP — Command queue interrupt pending
Bit 1: FIP — Fault queue interrupt pending
Bit 3: PIP — Page request queue interrupt pending
```

---

## 3. Data Structures

### 3.1 Device Directory Table (DDT)

The DDT is a radix tree indexed by `device_id`. Each leaf points to a
**Device Context (DC)** that holds per-device translation configuration.

```
 DDTP.PPN
     │
     ▼
 ┌─────────────────────────────────┐
 │  DDT Level 0 (page of entries)  │
 │  [0] DC for device_id 0        │
 │  [1] DC for device_id 1        │
 │  ...                            │
 │  [63] DC for device_id 63      │
 └─────────────────────────────────┘
```

For 1LVL mode (sufficient for QEMU virt), the entire DDT fits in one page.

### 3.2 Device Context (DC) — 64 bytes

Each Device Context configures how the IOMMU translates DMA for one device:

```c
struct riscv_iommu_dc {
    uint64_t tc;       /* 0x00: Translation Control         */
    uint64_t iohgatp;  /* 0x08: G-stage Address Translation */
    uint64_t ta;       /* 0x10: Translation Attributes      */
    uint64_t fsc;      /* 0x18: First-Stage Context         */
    uint64_t msiptp;   /* 0x20: MSI Page Table Pointer      */
    uint64_t msi_addr_mask;    /* 0x28 */
    uint64_t msi_addr_pattern; /* 0x30 */
    uint64_t _reserved;        /* 0x38 */
};
```

#### DC.tc — Translation Control
```
Bit 0:  V     — Valid (must be 1)
Bit 4:  DTF   — Disable Translation Faults (suppress fault reporting)
Bit 7:  GADE  — G-stage A/D bit hardware management
Bit 8:  SADE  — S-stage A/D bit hardware management
```

#### DC.iohgatp — G-stage (Second-Stage) Address Translation Pointer
```
 63    60 59    44 43                    0
┌────────┬────────┬──────────────────────┐
│  MODE  │ GSCID  │        PPN           │
└────────┴────────┴──────────────────────┘
```

| MODE | Value | Description              |
|------|-------|--------------------------|
| Bare | 0     | No G-stage translation   |
| Sv39x4 | 8  | 3-level, 39-bit VA (RV64)|
| Sv48x4 | 9  | 4-level, 48-bit VA       |
| Sv57x4 | 10 | 5-level, 57-bit VA       |

**Critical insight:** `iohgatp.PPN` should point to the **same root page
table** that Bao already uses for the VM's `hgatp` CSR. This means the
IOMMU enforces the same address translation as the CPU — DMA addresses
are translated through the VM's existing second-stage page tables.

**GSCID** is analogous to VMID — guest soft-context ID for TLB tagging.

#### DC.fsc — First-Stage Context
For our hypervisor use case, we only need G-stage (second-stage) translation.
Set `fsc.MODE = Bare` (0) to disable first-stage translation.

### 3.3 G-stage Page Tables (Sv39x4)

The IOMMU reuses the standard RISC-V page table format with the **x4**
extension: the first level has 4× the entries (11-bit index instead of 9).

```
Sv39x4 page table walk:

  Guest Physical Address (41 bits)
  ┌───────────┬──────────┬──────────┬────────────┐
  │ VPN[2] 11b│ VPN[1] 9b│ VPN[0] 9b│ offset 12b │
  └─────┬─────┴────┬─────┴────┬─────┴────────────┘
        │          │          │
        ▼          ▼          ▼
   L0: 2048     L1: 512    L2: 512
   entries      entries    entries
   (16 KiB)    (4 KiB)   (4 KiB)
```

**PTE format** (standard RISC-V, 8 bytes):
```
 63       54 53       10 9  8 7 6 5 4 3 2 1 0
┌───────────┬───────────┬────┬─┬─┬─┬─┬─┬─┬─┬─┐
│  reserved │    PPN    │RSW │D│A│G│U│X│W│R│V│
└───────────┴───────────┴────┴─┴─┴─┴─┴─┴─┴─┴─┘
```

**Key: The IOMMU walks the exact same page tables as the CPU H-extension.**
By pointing `DC.iohgatp.PPN` at the VM's root page table (same physical
address used in `hgatp`), DMA translations are automatically bounded to
the VM's assigned memory regions.

---

## 4. Architecture Overview

### 4.1 Initialization Flow

```
 Bao boot (CPU_MASTER)
       │
       ▼
 io_init()
       │
       ▼
 iommu_arch_init()
       │
       ├─ 1. Map IOMMU MMIO registers (0x3010000)
       ├─ 2. Read CAP register → verify Sv39x4, WSI support
       ├─ 3. Set FCTL.WSI = 1 (wired interrupts via PLIC)
       ├─ 4. Allocate DDT page (1LVL mode)
       ├─ 5. Set DDTP = { MODE=1LVL, PPN=ddt_page }
       ├─ 6. Allocate & configure Command Queue
       ├─ 7. Allocate & configure Fault Queue
       ├─ 8. Enable CQ (CQCSR.CQEN=1) and FQ (FQCSR.FQEN=1)
       └─ 9. Set DDTP.MODE from OFF/BARE → 1LVL (activates translation)
```

### 4.2 Per-VM Device Setup

```
 vm_init()
       │
       ▼
 io_vm_init(vm, config)
       │
       ▼
 iommu_arch_vm_init(vm, config)
       │
       ├─ Store vm->io.prot.mmu.gscid = vm->id
       └─ (prepare per-VM IOMMU state)

 For each device assigned to VM:
       │
       ▼
 io_vm_add_device(vm, stream_id)
       │
       ▼
 iommu_arch_vm_add_device(vm, stream_id)
       │
       ├─ 1. Get VM's root page table physical address
       │     (same as hgatp.PPN << 12)
       ├─ 2. Write Device Context in DDT[stream_id]:
       │       dc.tc     = { V=1, DTF=0 }
       │       dc.iohgatp = { MODE=Sv39x4, GSCID=vm_id, PPN=root_pt }
       │       dc.fsc    = { MODE=Bare }
       │       dc.ta     = { PSCID=0 }
       └─ 3. Issue IODIR.INVAL_DDT command to invalidate cached DC
```

### 4.3 Translation at Runtime

```
 Device DMA request (device_id, guest_addr)
       │
       ▼
 IOMMU hardware:
   1. Look up DDT[device_id] → Device Context
   2. If DC.V == 0 → FAULT (device not assigned to any VM)
   3. Walk DC.iohgatp page tables (same as VM's hgatp)
      guest_addr → physical_addr
   4. If translation succeeds → DMA proceeds to physical_addr
   5. If translation fails → FAULT record in Fault Queue
       │
       ▼
 PLIC IRQ 37 (FQ interrupt) → Bao handles fault
```

---

## 5. Implementation Plan

### 5.1 Files to Modify

| File | Changes |
|------|---------|
| `src/arch/riscv/inc/arch/platform.h` | Add `iommu` sub-struct to `struct arch_platform` |
| `src/arch/riscv/inc/arch/iommu.h` | IOMMU register map, DC struct, `struct iommu_vm_arch` |
| `src/arch/riscv/iommu.c` | Full IOMMU driver: init, vm_init, add_device |
| `src/arch/riscv/inc/arch/vm.h` | Add IOMMU fields to `struct arch_vm_platform` |
| `src/platform/qemu-riscv64-virt/virt_desc.c` | Add IOMMU base address to platform descriptor |
| `deploy.sh` (bao-demos) | Add `,iommu-sys=on` to QEMU machine option |

### 5.2 `struct arch_platform` Extension

```c
/* src/arch/riscv/inc/arch/platform.h */
struct arch_platform {
    paddr_t plic_base;

    struct {
        paddr_t base;         /* IOMMU MMIO base address */
    } iommu;
};
```

### 5.3 Platform Descriptor Update

```c
/* src/platform/qemu-riscv64-virt/virt_desc.c */
struct platform platform = {
    .cpu_num = 4,
    .region_num = 1,
    .regions = (struct mem_region[]) {
        { .base = 0x80200000, .size = 0x100000000 - 0x200000 }
    },
    .arch = {
        .plic_base = 0xc000000,
        .iommu = {
            .base = 0x3010000,    /* From QEMU DTB */
        },
    }
};
```

### 5.4 IOMMU Header (New Definitions)

```c
/* src/arch/riscv/inc/arch/iommu.h */

/* ── Register offsets ──────────────────────────── */
#define RISCV_IOMMU_REG_CAP      0x0000
#define RISCV_IOMMU_REG_FCTL     0x0008
#define RISCV_IOMMU_REG_DDTP     0x0010
#define RISCV_IOMMU_REG_CQB      0x0018
#define RISCV_IOMMU_REG_CQH      0x0020
#define RISCV_IOMMU_REG_CQT      0x0024
#define RISCV_IOMMU_REG_FQB      0x0028
#define RISCV_IOMMU_REG_FQH      0x0030
#define RISCV_IOMMU_REG_FQT      0x0034
#define RISCV_IOMMU_REG_CQCSR    0x0048
#define RISCV_IOMMU_REG_FQCSR    0x004C
#define RISCV_IOMMU_REG_IPSR     0x0054

/* ── DDTP modes ────────────────────────────────── */
#define RISCV_IOMMU_DDTP_MODE_OFF    0
#define RISCV_IOMMU_DDTP_MODE_BARE   1
#define RISCV_IOMMU_DDTP_MODE_1LVL   2
#define RISCV_IOMMU_DDTP_MODE_2LVL   3
#define RISCV_IOMMU_DDTP_MODE_3LVL   4

/* ── iohgatp modes ─────────────────────────────── */
#define RISCV_IOMMU_IOHGATP_SV39X4   8
#define RISCV_IOMMU_IOHGATP_SV48X4   9
#define RISCV_IOMMU_IOHGATP_SV57X4  10

/* ── FCTL bits ─────────────────────────────────── */
#define RISCV_IOMMU_FCTL_WSI   (1UL << 1)

/* ── CQCSR bits ────────────────────────────────── */
#define RISCV_IOMMU_CQCSR_CQEN  (1UL << 0)
#define RISCV_IOMMU_CQCSR_CQON  (1UL << 16)

/* ── FQCSR bits ────────────────────────────────── */
#define RISCV_IOMMU_FQCSR_FQEN  (1UL << 0)
#define RISCV_IOMMU_FQCSR_FQON  (1UL << 16)

/* ── DC.tc bits ────────────────────────────────── */
#define RISCV_IOMMU_DC_TC_V     (1ULL << 0)
#define RISCV_IOMMU_DC_TC_DTF   (1ULL << 4)
#define RISCV_IOMMU_DC_TC_GADE  (1ULL << 7)
#define RISCV_IOMMU_DC_TC_SADE  (1ULL << 8)

/* ── Command opcodes ───────────────────────────── */
#define RISCV_IOMMU_CMD_IOFENCE_C       0x002  /* opcode=2, func=0 */
#define RISCV_IOMMU_CMD_IODIR_INVAL_DDT 0x003  /* opcode=3, func=0 */
#define RISCV_IOMMU_CMD_IOTINVAL_VMA    0x001  /* opcode=1, func=0 */
#define RISCV_IOMMU_CMD_IOTINVAL_GVMA   0x081  /* opcode=1, func=1 */

/* ── Device Context structure (64 bytes) ───────── */
struct riscv_iommu_dc {
    uint64_t tc;
    uint64_t iohgatp;
    uint64_t ta;
    uint64_t fsc;
    uint64_t msiptp;
    uint64_t msi_addr_mask;
    uint64_t msi_addr_pattern;
    uint64_t _reserved;
};

/* ── Per-VM IOMMU state ────────────────────────── */
struct iommu_vm_arch {
    ssize_t gscid;       /* Guest Soft-Context ID (= vm_id) */
};
```

### 5.5 IOMMU Driver Pseudocode

```c
/* src/arch/riscv/iommu.c — key functions */

static volatile void *iommu_base;  /* Mapped MMIO base */
static struct riscv_iommu_dc *ddt; /* Device Directory Table (1 page) */

/* ── Command Queue ──────────────────────────────── */
static struct {
    uint64_t (*entries)[2];  /* 128-bit commands */
    size_t size;             /* Number of entries */
} cmd_queue;

bool iommu_arch_init(void)
{
    if (cpu()->id != CPU_MASTER || !platform.arch.iommu.base)
        return false;

    /* 1. Map IOMMU MMIO registers */
    iommu_base = (void *)mem_alloc_map_dev(
        &cpu()->as, SEC_HYP_GLOBAL,
        platform.arch.iommu.base, PAGE_SIZE);

    /* 2. Read capabilities */
    uint64_t cap = read64(iommu_base + RISCV_IOMMU_REG_CAP);
    /* Verify Sv39x4 support: cap bit 17 */

    /* 3. Enable wired signaled interrupts (for PLIC) */
    write32(iommu_base + RISCV_IOMMU_REG_FCTL, RISCV_IOMMU_FCTL_WSI);

    /* 4. Allocate DDT (one 4K page for 1LVL, holds 64 DCs) */
    ddt = page_alloc_aligned(1);  /* zero-filled */

    /* 5. Allocate Command Queue (e.g., 16 entries) */
    cmd_queue.entries = page_alloc_aligned(1);
    cmd_queue.size = 16;
    uint64_t cqb = (virt_to_phys(cmd_queue.entries) >> 12) << 10
                  | (ilog2(cmd_queue.size) - 1);
    write64(iommu_base + RISCV_IOMMU_REG_CQB, cqb);

    /* 6. Allocate Fault Queue (e.g., 16 entries) */
    /* Similar to CQ... */

    /* 7. Enable command and fault queues */
    write32(iommu_base + RISCV_IOMMU_REG_CQCSR, RISCV_IOMMU_CQCSR_CQEN);
    write32(iommu_base + RISCV_IOMMU_REG_FQCSR, RISCV_IOMMU_FQCSR_FQEN);

    /* 8. Set DDTP to 1LVL mode - activates translation */
    uint64_t ddtp = (virt_to_phys(ddt) >> 12) << 10
                  | RISCV_IOMMU_DDTP_MODE_1LVL;
    write64(iommu_base + RISCV_IOMMU_REG_DDTP, ddtp);

    return true;
}

bool iommu_arch_vm_add_device(struct vm *vm, streamid_t id)
{
    if (!iommu_base || id >= 64)  /* 1LVL limit */
        return false;

    /* Get VM's root page table physical address (same as hgatp) */
    paddr_t root_pt;
    mem_translate(&cpu()->as, (vaddr_t)vm->as.pt.root, &root_pt);

    /* Write Device Context */
    struct riscv_iommu_dc *dc = &ddt[id];
    dc->tc     = RISCV_IOMMU_DC_TC_V;
    dc->iohgatp = ((uint64_t)RISCV_IOMMU_IOHGATP_SV39X4 << 60)
                 | ((uint64_t)(vm->id & 0xFFFF) << 44)
                 | (root_pt >> 12);
    dc->fsc    = 0;  /* Bare — no first-stage translation */
    dc->ta     = 0;

    /* Fence: ensure DC is visible, then invalidate cached DC */
    __sync_synchronize();
    iommu_submit_cmd_iodir_inval_ddt(id);
    iommu_submit_cmd_iofence();

    return true;
}

bool iommu_arch_vm_init(struct vm *vm, const struct vm_config *config)
{
    vm->io.prot.mmu.gscid = vm->id;
    return true;
}
```

### 5.6 deploy.sh Change

```diff
- QEMU_MACHINE=${QEMU_MACHINE:-virt}
+ QEMU_MACHINE=${QEMU_MACHINE:-virt,iommu-sys=on}
```

---

## 6. Command Queue Operations

The IOMMU processes commands from a circular queue in memory. The hypervisor
writes commands to `CQ[CQT]` and increments the tail pointer. The IOMMU
hardware reads from `CQ[CQH]` and increments the head pointer.

### 6.1 IODIR.INVAL_DDT — Invalidate Device Context Cache

```
dword0: opcode=3, func=0, DV=1, DID=device_id
dword1: 0
```

Used after writing/updating a Device Context in the DDT.

### 6.2 IOFENCE.C — Completion Fence

```
dword0: opcode=2, func=0
dword1: 0
```

Ensures all preceding commands have completed. Used after DDT invalidation
to guarantee the IOMMU sees the new Device Context before DMA can proceed.

### 6.3 IOTINVAL.GVMA — Invalidate G-stage TLB

```
dword0: opcode=1, func=1, GV=1, GSCID=vm_id
dword1: 0 (all addresses) or specific IOVA
```

Used if VM page tables are modified at runtime (e.g., dynamic device
passthrough). Invalidates cached G-stage translations for a specific GSCID.

---

## 7. Fault Handling

When a DMA translation fails (unmapped address, invalid DC, permission
violation), the IOMMU writes a 32-byte fault record to the Fault Queue
and raises PLIC IRQ 37.

### 7.1 Fault Record Format

```c
struct riscv_iommu_fq_record {
    uint64_t hdr;       /* cause[11:0], PID, TTYPE, DID */
    uint64_t _reserved;
    uint64_t iotval;    /* Faulting IOVA */
    uint64_t iotval2;   /* Additional info */
};
```

### 7.2 Fault Causes (Common)

| Cause | Description                          |
|-------|--------------------------------------|
| 0     | Instruction access fault             |
| 1     | Read access fault                    |
| 3     | Write/AMO access fault               |
| 259   | DDT entry not valid                  |
| 260   | DDT entry misconfigured              |
| 265   | G-stage PTE not valid                |
| 266   | G-stage PTE read access fault        |
| 267   | G-stage PTE write access fault       |

### 7.3 Fault Handling Strategy

For MCS, the fault handler should:

1. **Read fault record** from FQ[FQH]
2. **Identify the faulting device** via DID field
3. **Log the fault** with IOVA and cause
4. **Look up which VM owns DID** → determine criticality
5. **Policy decision:**
   - CRIT_HIGH device fault → **system-level alert** (should not happen
     in a correctly configured system — indicates hardware/config error)
   - CRIT_LOW device fault → **isolate the device** (set DC.V = 0),
     mark VM as unhealthy via health monitor integration
6. **Advance FQH** to consume the record
7. **Clear IPSR.FIP** (W1C) to acknowledge interrupt

---

## 8. Integration with Existing Features

### 8.1 Criticality-Aware Device Assignment

The existing `struct vm_dev_region` already has a `streamid_t id` field
for IOMMU effects. Combined with per-interrupt criticality metadata:

```c
struct vm_dev_region {
    paddr_t pa;
    vaddr_t va;
    size_t size;
    size_t interrupt_num;
    struct vm_irq *interrupts;  /* Per-IRQ criticality */
    streamid_t id;              /* Device ID for IOMMU */
};
```

No config struct changes needed — the existing `id` field is already
designed for this purpose.

### 8.2 Health Monitor Integration

IOMMU faults for CRIT_LOW devices could feed into the health monitor:
- A DMA fault indicates the guest is misbehaving (programming devices
  with invalid addresses)
- The fault handler could call `health_monitor_mark_unhealthy(vm_id)`
- This integrates DMA isolation with the existing health monitoring system

### 8.3 IRQ Rate Limiting

The IOMMU's fault interrupt (PLIC IRQ 37) should be rate-limited to
prevent a malicious guest from flooding the hypervisor with DMA faults.
The existing token-bucket rate limiter can be applied to IRQ 37.

---

## 9. Testing Strategy

### 9.1 Unit Tests

| # | Test | Description |
|---|------|-------------|
| 1 | `test_iommu_init` | Verify IOMMU init reads CAP, sets DDTP to 1LVL |
| 2 | `test_iommu_add_device` | Add device, verify DC written correctly |
| 3 | `test_iommu_dc_valid` | Check DC.V=1, iohgatp points to VM's PT |
| 4 | `test_iommu_dc_gscid` | Verify GSCID matches VM ID |
| 5 | `test_iommu_unassigned_fault` | Unassigned device_id → DC.V=0 → fault |
| 6 | `test_iommu_multiple_vms` | Two VMs with different devices, isolated |
| 7 | `test_iommu_cmd_queue` | Command submission and fence completion |

### 9.2 Integration Tests (on QEMU)

1. **Basic DMA isolation:** Boot 2 VMs, assign different devices, verify
   each VM can only DMA to its own memory regions
2. **Fault generation:** Program a device with an address outside VM's
   memory → verify fault record appears in FQ
3. **Cross-VM DMA attempt:** VM1 tries to DMA to VM0's physical range →
   verify IOMMU blocks it

### 9.3 QEMU Verification

```bash
# Launch with IOMMU enabled
./deploy.sh  # (after adding iommu-sys=on to QEMU_MACHINE)

# In QEMU monitor, verify IOMMU state:
# (Ctrl+A, C for monitor)
info mtree    # Should show IOMMU MMIO region at 0x3010000
```

---

## 10. MCS Considerations & Limitations

### 10.1 Determinism

The IOMMU adds latency to every DMA transaction (page table walk). For
CRIT_HIGH devices, this must be factored into WCET analysis:
- **IOMMU TLB hit:** Near-zero additional latency
- **IOMMU TLB miss:** Page table walk (2-3 memory accesses for Sv39x4)

The IOTLB walk is bounded and deterministic (fixed number of levels),
which is acceptable for MCS timing analysis.

### 10.2 Shared IOTLB Contention

All devices share the IOMMU's IOTLB. A CRIT_LOW device generating many
DMA transactions could evict IOTLB entries for a CRIT_HIGH device. The
RISC-V IOMMU spec does not define IOTLB partitioning.

**Mitigation:** GSCID-based tagging helps, but does not guarantee
partition isolation in the IOTLB. This is a known limitation — hardware
IOTLB QoS is not available in the current spec.

### 10.3 No Bandwidth Throttling

The IOMMU translates addresses but does not limit DMA bandwidth. A
CRIT_LOW device can still saturate the memory bus. DMA bandwidth control
requires platform-specific QoS mechanisms (e.g., RISC-V QoS extensions)
which are outside the IOMMU scope.

### 10.4 Static Device Assignment Only

This implementation supports static device-to-VM assignment at boot time.
Dynamic device hotplug or migration between VMs is not supported. This
matches Bao's partitioning philosophy.

---

## 11. Reference: ARM SMMUv2 Pattern in Bao

The existing ARM SMMUv2 driver (`src/arch/armv8/armv8-a/iommu.c`) follows
the same architectural pattern we implement for RISC-V:

| Concept | ARM SMMUv2 | RISC-V IOMMU |
|---------|-----------|--------------|
| Device lookup | Stream Match Register (SMR) | Device Directory Table (DDT) |
| Per-device context | Context Bank | Device Context (DC) |
| Page table reuse | `rootpt` → context bank TTBR0 | `rootpt` → DC.iohgatp.PPN |
| VM identifier | VMID in CBAR | GSCID in DC.iohgatp |
| Context allocation | `smmu_alloc_ctxbnk()` | DDT index = device_id |
| Invalidation | TLBIALLNSNH | IODIR.INVAL_DDT + IOFENCE.C |
| Platform config | `platform.arch.smmu.base` | `platform.arch.iommu.base` |

The RISC-V IOMMU is simpler in some ways: there's no separate
context-bank allocation because the DDT directly indexes by device_id.

---

## 12. Summary

| Component | Action |
|-----------|--------|
| QEMU launch | Add `iommu-sys=on` to `-M virt` |
| Platform descriptor | Add `iommu.base = 0x3010000` |
| `arch_platform` struct | Add `struct { paddr_t base; } iommu` |
| `iommu.h` | Register map, DC struct, per-VM state |
| `iommu.c` | Full driver: init, add_device, vm_init, cmd queue |
| Fault handling | FQ drain, log, criticality-aware policy |
| Integration | Reuse VM's hgatp page tables for DMA translation |
| Testing | 7 unit tests + 3 integration tests on QEMU |
