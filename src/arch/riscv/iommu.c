/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

/**
 * @file iommu.c
 * @brief RISC-V IOMMU driver — DMA filtration for Mixed-Criticality Systems.
 *
 * Translates device DMA through the same second-stage page tables the CPU
 * already uses (hgatp), so each VM's DMA is hardware-confined to the VM's
 * assigned physical memory.  Uses 1LVL DDT mode (one page of Device
 * Contexts, supporting up to 64 device_ids — more than enough for QEMU virt).
 *
 * Initialization sequence:
 *   1. Map IOMMU MMIO
 *   2. Read CAP — verify Sv39x4 + WSI support
 *   3. Enable WSI (for PLIC-based interrupts)
 *   4. Allocate DDT page, zero it (all DCs invalid → DMA faulted)
 *   5. Allocate + configure Command Queue and Fault Queue
 *   6. Enable CQ and FQ
 *   7. Write DDTP with MODE=1LVL (activates translation)
 *
 * Per-device setup (called from io_vm_add_device):
 *   1. Translate VM's page-table root to a physical address
 *   2. Write DC: valid, iohgatp = Sv39x4 + GSCID + PT root PPN
 *   3. Issue IODIR.INVAL_DDT + IOFENCE to flush IOMMU caches
 */

#include <io.h>
#include <arch/iommu.h>
#include <cpu.h>
#include <mem.h>
#include <platform.h>
#include <vm.h>
#include <fences.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Private state
 * ═══════════════════════════════════════════════════════════════════════════ */

/** MMIO base virtual address (NULL if no IOMMU on this platform). */
static volatile uint8_t *iommu_base;

/** DDT page — array of Device Contexts indexed by device_id. */
static volatile struct riscv_iommu_dc *ddt;

/** Physical address of DDT page (needed for DDTP register). */
static paddr_t ddt_phys;

/* Command queue (CQ): 16 entries × 16 bytes = 256 bytes, fits in 1 page. */
#define CQ_LOG2SZ  4   /* 2^4 = 16 entries */
#define CQ_NUM     (1U << CQ_LOG2SZ)

static volatile uint64_t (*cq_entries)[2]; /* 128-bit commands */
static paddr_t cq_phys;

/* Fault queue (FQ): 16 entries × 32 bytes = 512 bytes, fits in 1 page. */
#define FQ_LOG2SZ  4
#define FQ_NUM     (1U << FQ_LOG2SZ)

static volatile struct riscv_iommu_fq_record *fq_entries;
static paddr_t fq_phys;

/* ═══════════════════════════════════════════════════════════════════════════
 * Low-level MMIO helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t iommu_read32(size_t off)
{
    return *(volatile uint32_t *)(iommu_base + off);
}

static inline void iommu_write32(size_t off, uint32_t val)
{
    *(volatile uint32_t *)(iommu_base + off) = val;
}

static inline uint64_t iommu_read64(size_t off)
{
    return *(volatile uint64_t *)(iommu_base + off);
}

static inline void iommu_write64(size_t off, uint64_t val)
{
    *(volatile uint64_t *)(iommu_base + off) = val;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Command Queue helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Submit a 128-bit command to the command queue and advance the tail.
 * Spins if the queue is full (CQT+1 == CQH).
 */
static void iommu_submit_cmd(uint64_t dword0, uint64_t dword1)
{
    uint32_t tail = iommu_read32(RISCV_IOMMU_REG_CQT);
    uint32_t next = (tail + 1) & (CQ_NUM - 1);

    /* Spin-wait if queue is full — should never happen at boot. */
    while (next == iommu_read32(RISCV_IOMMU_REG_CQH)) {
        ;
    }

    cq_entries[tail][0] = dword0;
    cq_entries[tail][1] = dword1;
    fence_sync_write();

    iommu_write32(RISCV_IOMMU_REG_CQT, next);
}

/**
 * Issue IODIR.INVAL_DDT for a specific device_id.
 * Invalidates any cached Device Context for this device.
 */
static void iommu_cmd_inval_ddt(uint32_t device_id)
{
    uint64_t dword0 = RISCV_IOMMU_CMD_IODIR_INVAL_DDT_OPCODE
                     | RISCV_IOMMU_CMD_IODIR_DV
                     | ((uint64_t)device_id << RISCV_IOMMU_CMD_IODIR_DID_OFF);
    iommu_submit_cmd(dword0, 0);
}

/**
 * Issue IOFENCE.C — wait for all prior commands to complete.
 * After return, the IOMMU has processed all preceding commands.
 */
static void iommu_cmd_iofence(void)
{
    iommu_submit_cmd(RISCV_IOMMU_CMD_IOFENCE_OPCODE, 0);

    /* Wait for the IOMMU to consume the fence command (head catches tail). */
    uint32_t tail = iommu_read32(RISCV_IOMMU_REG_CQT);
    while (iommu_read32(RISCV_IOMMU_REG_CQH) != tail) {
        ;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Fault Queue drain — read and log all pending IOMMU fault records
 * ═══════════════════════════════════════════════════════════════════════════ */

/** IOMMU fault PLIC IRQ (first of 36-39 on QEMU virt) */
#define IOMMU_FQ_IRQ  36

static void iommu_drain_fq(void)
{
    if (!iommu_base || !fq_entries) return;

    uint32_t head = iommu_read32(RISCV_IOMMU_REG_FQH);
    uint32_t tail = iommu_read32(RISCV_IOMMU_REG_FQT);

    while (head != tail) {
        volatile struct riscv_iommu_fq_record *rec = &fq_entries[head];
        uint64_t cause = rec->hdr & RISCV_IOMMU_FQ_HDR_CAUSE_MSK;
        uint64_t did   = (rec->hdr & RISCV_IOMMU_FQ_HDR_DID_MSK)
                          >> RISCV_IOMMU_FQ_HDR_DID_OFF;

        INFO("[BENCH:IOMMU:FAULT] cause=%lu dev=%lu iotval=0x%lx iotval2=0x%lx",
             (unsigned long)cause, (unsigned long)did,
             (unsigned long)rec->iotval, (unsigned long)rec->iotval2);

        head = (head + 1) & (FQ_NUM - 1);
    }

    /* Advance head so hardware can reuse the slots */
    iommu_write32(RISCV_IOMMU_REG_FQH, head);

    /* Clear fault-interrupt-pending bit in IPSR */
    iommu_write32(RISCV_IOMMU_REG_IPSR, RISCV_IOMMU_IPSR_FIP);
}

/* Callable from timer interrupt path or externally for on-demand drain. */
void iommu_check_faults(void)
{
    iommu_drain_fq();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * IOMMU Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

bool iommu_arch_init(void)
{
#ifdef BENCH_NO_IOMMU
    return true;  /* Benchmark: skip IOMMU — DMA passes through */
#endif
    /* Only master CPU initializes the IOMMU, and only if platform has one. */
    if (cpu()->id != CPU_MASTER || platform.arch.iommu.base == 0) {
        return true;        /* No IOMMU — graceful fallback (pass-through). */
    }

    /* ── 1. Map IOMMU MMIO registers ────────────────────────────────────── */
    iommu_base = (volatile uint8_t *)mem_alloc_map_dev(
        &cpu()->as, SEC_HYP_GLOBAL, INVALID_VA,
        platform.arch.iommu.base, NUM_PAGES(PAGE_SIZE));

    if (!iommu_base) {
        WARNING("iommu: failed to map IOMMU MMIO at 0x%lx",
                platform.arch.iommu.base);
        return false;
    }

    /* ── 1b. Ensure DDTP is in Bare (pass-through) mode ─────────────────
     * The spec resets DDTP to mode=0 (Off/Bare), but we write it
     * explicitly as a safety measure — all DMA passes through until
     * we finish configuring DDT, CQ, FQ and switch to 1LVL below. */
    iommu_write64(RISCV_IOMMU_REG_DDTP, RISCV_IOMMU_DDTP_MODE_BARE);
    while (iommu_read64(RISCV_IOMMU_REG_DDTP) & RISCV_IOMMU_DDTP_BUSY) {
        ;
    }

    /* ── 2. Read capabilities ───────────────────────────────────────────── */
    uint64_t cap = iommu_read64(RISCV_IOMMU_REG_CAP);

    if (!(cap & RISCV_IOMMU_CAP_SV39X4)) {
        WARNING("iommu: Sv39x4 not supported (CAP=0x%lx)", cap);
        return false;
    }

    uint64_t igs = (cap & RISCV_IOMMU_CAP_IGS_MSK) >> RISCV_IOMMU_CAP_IGS_OFF;
    if (igs != RISCV_IOMMU_CAP_IGS_WSI && igs != RISCV_IOMMU_CAP_IGS_BOTH) {
        WARNING("iommu: WSI not supported (IGS=%lu)", igs);
        return false;
    }

    INFO("iommu: RISC-V IOMMU at 0x%lx, CAP=0x%lx",
         platform.arch.iommu.base, cap);

    /* ── 3. Enable wired-signaled interrupts (for PLIC) ─────────────────── */
    iommu_write32(RISCV_IOMMU_REG_FCTL, RISCV_IOMMU_FCTL_WSI);

    /* ── 4. Allocate DDT (1 page → 64 DCs, all zeroed = invalid) ────────── */
    ddt = (volatile struct riscv_iommu_dc *)mem_alloc_page(1, SEC_HYP_GLOBAL, true);
    if (!ddt) {
        WARNING("iommu: failed to allocate DDT page");
        return false;
    }
    /* Zero the DDT — all DCs start with V=0 (invalid → DMA faulted). */
    for (size_t i = 0; i < RISCV_IOMMU_DDT_1LVL_MAX_DEVS; i++) {
        ddt[i].tc = 0;
        ddt[i].iohgatp = 0;
        ddt[i].ta = 0;
        ddt[i].fsc = 0;
        ddt[i].msiptp = 0;
        ddt[i].msi_addr_mask = 0;
        ddt[i].msi_addr_pattern = 0;
        ddt[i]._reserved = 0;
    }
    mem_translate(&cpu()->as, (vaddr_t)ddt, &ddt_phys);

    /* ── 5. Allocate Command Queue ──────────────────────────────────────── */
    cq_entries = (volatile uint64_t (*)[2])mem_alloc_page(1, SEC_HYP_GLOBAL, true);
    if (!cq_entries) {
        WARNING("iommu: failed to allocate command queue");
        return false;
    }
    mem_translate(&cpu()->as, (vaddr_t)cq_entries, &cq_phys);

    uint64_t cqb = ((cq_phys >> 12) << RISCV_IOMMU_QXB_PPN_OFF)
                  | ((CQ_LOG2SZ - 1) & RISCV_IOMMU_QXB_LOG2SZ_MSK);
    iommu_write64(RISCV_IOMMU_REG_CQB, cqb);
    iommu_write32(RISCV_IOMMU_REG_CQH, 0);
    iommu_write32(RISCV_IOMMU_REG_CQT, 0);

    /* ── 6. Allocate Fault Queue ────────────────────────────────────────── */
    fq_entries = (volatile struct riscv_iommu_fq_record *)
        mem_alloc_page(1, SEC_HYP_GLOBAL, true);
    if (!fq_entries) {
        WARNING("iommu: failed to allocate fault queue");
        return false;
    }
    mem_translate(&cpu()->as, (vaddr_t)fq_entries, &fq_phys);

    uint64_t fqb = ((fq_phys >> 12) << RISCV_IOMMU_QXB_PPN_OFF)
                  | ((FQ_LOG2SZ - 1) & RISCV_IOMMU_QXB_LOG2SZ_MSK);
    iommu_write64(RISCV_IOMMU_REG_FQB, fqb);
    iommu_write32(RISCV_IOMMU_REG_FQH, 0);
    iommu_write32(RISCV_IOMMU_REG_FQT, 0);

    /* ── 7. Enable Command Queue and Fault Queue ────────────────────────── */
    iommu_write32(RISCV_IOMMU_REG_CQCSR, RISCV_IOMMU_CQCSR_CQEN);
    /* Spin until CQON is asserted. */
    while (!(iommu_read32(RISCV_IOMMU_REG_CQCSR) & RISCV_IOMMU_CQCSR_CQON)) {
        ;
    }

    iommu_write32(RISCV_IOMMU_REG_FQCSR, RISCV_IOMMU_FQCSR_FQEN);
    while (!(iommu_read32(RISCV_IOMMU_REG_FQCSR) & RISCV_IOMMU_FQCSR_FQON)) {
        ;
    }

    /* ── 8. Set DDTP to 1LVL — activates DMA address translation ────────── */
    uint64_t ddtp = ((ddt_phys >> 12) << RISCV_IOMMU_DDTP_PPN_OFF)
                  | RISCV_IOMMU_DDTP_MODE_1LVL;
    iommu_write64(RISCV_IOMMU_REG_DDTP, ddtp);

    /* Wait for DDTP.BUSY to clear. */
    while (iommu_read64(RISCV_IOMMU_REG_DDTP) & RISCV_IOMMU_DDTP_BUSY) {
        ;
    }

    /* ── 9. Fault-queue interrupt ────────────────────────────────────────
     * NOTE: We keep FIE disabled and drain the fault queue on-demand
     * (from the timer path or DDT dump) to avoid PLIC IRQ conflicts
     * with the global_interrupt_bitmap during VM device assignment.   */

    INFO("iommu: initialized — DDT at PA 0x%lx, mode=1LVL, CQ=%u FQ=%u entries",
         ddt_phys, CQ_NUM, FQ_NUM);

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Per-VM IOMMU initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

bool iommu_arch_vm_init(struct vm *vm, const struct vm_config *config)
{
    if (!iommu_base) {
        return true;   /* No IOMMU present — nothing to do. */
    }

    /* Assign GSCID = VM ID for IOTLB tagging. */
    vm->io.prot.mmu.gscid = (ssize_t)vm->id;

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Add a device to a VM's IOMMU domain
 * ═══════════════════════════════════════════════════════════════════════════ */

bool iommu_arch_vm_add_device(struct vm *vm, streamid_t id)
{
    if (!iommu_base) {
        return true;   /* No IOMMU present — pass-through. */
    }

    if ((size_t)id >= RISCV_IOMMU_DDT_1LVL_MAX_DEVS) {
        WARNING("iommu: device_id %u exceeds 1LVL DDT capacity (%lu)",
                id, RISCV_IOMMU_DDT_1LVL_MAX_DEVS);
        return false;
    }

    /* Get the physical address of the VM's root page table.
     * This is the same page table used for hgatp CPU 2nd-stage translation. */
    paddr_t root_pt_phys;
    mem_translate(&cpu()->as, (vaddr_t)vm->as.pt.root, &root_pt_phys);

    uint64_t gscid = (uint64_t)vm->io.prot.mmu.gscid & 0xFFFF;

    /* ── Write Device Context ───────────────────────────────────────────── */
    volatile struct riscv_iommu_dc *dc = &ddt[id];

    dc->fsc            = 0;  /* Bare — no first-stage translation */
    dc->ta             = 0;  /* PSCID = 0, not used */
    dc->msiptp         = 0;
    dc->msi_addr_mask  = 0;
    dc->msi_addr_pattern = 0;
    dc->_reserved      = 0;

    /* iohgatp: MODE=Sv39x4, GSCID=vm_id, PPN=root page table */
    dc->iohgatp = (RISCV_IOMMU_IOHGATP_SV39X4 << RISCV_IOMMU_DC_IOHGATP_MODE_OFF)
                | (gscid << RISCV_IOMMU_DC_IOHGATP_GSCID_OFF)
                | ((root_pt_phys >> 12) & RISCV_IOMMU_DC_IOHGATP_PPN_MSK);

    /* Write tc LAST — setting V=1 activates this DC. */
    fence_sync_write();
    dc->tc = RISCV_IOMMU_DC_TC_V;
    fence_sync_write();

    /* ── Invalidate IOMMU caches for this device ────────────────────────── */
    iommu_cmd_inval_ddt(id);
    iommu_cmd_iofence();

    INFO("iommu: device %u → VM %d (GSCID=%lu, root_pt=0x%lx)",
         id, vm->id, gscid, root_pt_phys);

    return true;
}
