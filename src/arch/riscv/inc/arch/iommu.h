/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

/**
 * @file iommu.h
 * @brief RISC-V IOMMU driver for DMA isolation in Mixed-Criticality Systems.
 *
 * Implements the RISC-V IOMMU specification (ratified, v1.0) to enforce
 * per-VM DMA address translation.  Each device assigned to a VM gets a
 * Device Context whose iohgatp points at the VM's existing second-stage
 * page tables — the same ones used by hgatp for CPU translation — so DMA
 * is automatically bounded to the VM's assigned physical memory.
 *
 * See docs/design-iommu-dma.md for the full design plan.
 */

#ifndef __IOMMU_ARCH_H__
#define __IOMMU_ARCH_H__

#include <bao.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * MMIO register offsets (from IOMMU base address)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RISCV_IOMMU_REG_CAP        0x0000  /* Capabilities (64-bit, RO)          */
#define RISCV_IOMMU_REG_FCTL       0x0008  /* Feature control (32-bit, RW)       */
#define RISCV_IOMMU_REG_DDTP       0x0010  /* Device Directory Table Ptr (64-bit)*/
#define RISCV_IOMMU_REG_CQB        0x0018  /* Command Queue Base (64-bit)        */
#define RISCV_IOMMU_REG_CQH        0x0020  /* Command Queue Head (32-bit)        */
#define RISCV_IOMMU_REG_CQT        0x0024  /* Command Queue Tail (32-bit)        */
#define RISCV_IOMMU_REG_FQB        0x0028  /* Fault Queue Base (64-bit)          */
#define RISCV_IOMMU_REG_FQH        0x0030  /* Fault Queue Head (32-bit)          */
#define RISCV_IOMMU_REG_FQT        0x0034  /* Fault Queue Tail (32-bit)          */
#define RISCV_IOMMU_REG_CQCSR      0x0048  /* Command Queue CSR (32-bit)         */
#define RISCV_IOMMU_REG_FQCSR      0x004C  /* Fault Queue CSR (32-bit)           */
#define RISCV_IOMMU_REG_IPSR       0x0054  /* Interrupt Pending Status (32-bit)  */

/* ═══════════════════════════════════════════════════════════════════════════
 * Capabilities register (CAP) bit fields
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RISCV_IOMMU_CAP_SV39X4     (1ULL << 17)
#define RISCV_IOMMU_CAP_SV48X4     (1ULL << 18)
#define RISCV_IOMMU_CAP_SV57X4     (1ULL << 19)
#define RISCV_IOMMU_CAP_IGS_OFF    28
#define RISCV_IOMMU_CAP_IGS_MSK    (0x3ULL << RISCV_IOMMU_CAP_IGS_OFF)
#define RISCV_IOMMU_CAP_IGS_WSI    1  /* Wired-signaled interrupts supported */
#define RISCV_IOMMU_CAP_IGS_BOTH   2  /* Both MSI and WSI supported          */

/* ═══════════════════════════════════════════════════════════════════════════
 * Feature Control (FCTL) bits
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RISCV_IOMMU_FCTL_WSI       (1UL << 1)   /* Enable wired interrupts    */

/* ═══════════════════════════════════════════════════════════════════════════
 * DDTP — Device Directory Table Pointer
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RISCV_IOMMU_DDTP_MODE_MSK   0xFULL
#define RISCV_IOMMU_DDTP_MODE_BARE  0   /* Off / pass-through — no translation */
#define RISCV_IOMMU_DDTP_MODE_1LVL  2
#define RISCV_IOMMU_DDTP_MODE_2LVL  3
#define RISCV_IOMMU_DDTP_MODE_3LVL  4
#define RISCV_IOMMU_DDTP_BUSY       (1ULL << 4)
#define RISCV_IOMMU_DDTP_PPN_OFF    10
#define RISCV_IOMMU_DDTP_PPN_MSK    (0xFFFFFFFFFFFULL << RISCV_IOMMU_DDTP_PPN_OFF)

/* ═══════════════════════════════════════════════════════════════════════════
 * Command Queue CSR (CQCSR) bits
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RISCV_IOMMU_CQCSR_CQEN     (1UL << 0)
#define RISCV_IOMMU_CQCSR_CIE      (1UL << 1)
#define RISCV_IOMMU_CQCSR_CQON     (1UL << 16)
#define RISCV_IOMMU_CQCSR_BUSY     (1UL << 17)

/* ═══════════════════════════════════════════════════════════════════════════
 * Fault Queue CSR (FQCSR) bits
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RISCV_IOMMU_FQCSR_FQEN     (1UL << 0)
#define RISCV_IOMMU_FQCSR_FIE      (1UL << 1)
#define RISCV_IOMMU_FQCSR_FQON     (1UL << 16)
#define RISCV_IOMMU_FQCSR_BUSY     (1UL << 17)

/* ═══════════════════════════════════════════════════════════════════════════
 * IPSR bits
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RISCV_IOMMU_IPSR_CIP       (1UL << 0)
#define RISCV_IOMMU_IPSR_FIP       (1UL << 1)

/* ═══════════════════════════════════════════════════════════════════════════
 * Queue base register encoding:  [53:10] = PPN, [4:0] = LOG2SZ-1
 * Number of entries = 2^(LOG2SZ).
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RISCV_IOMMU_QXB_PPN_OFF    10
#define RISCV_IOMMU_QXB_LOG2SZ_MSK 0x1FULL

/* ═══════════════════════════════════════════════════════════════════════════
 * Device Context (DC) — 64 bytes
 * Stored in DDT, one per device_id, configures DMA translation.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct riscv_iommu_dc {
    uint64_t tc;               /* Translation Control                     */
    uint64_t iohgatp;          /* G-stage (2nd-stage) Address Translation */
    uint64_t ta;               /* Translation Attributes                  */
    uint64_t fsc;              /* First-Stage Context (S-stage/PDT)       */
    uint64_t msiptp;           /* MSI Page Table Pointer                  */
    uint64_t msi_addr_mask;    /* MSI address mask                        */
    uint64_t msi_addr_pattern; /* MSI address pattern                     */
    uint64_t _reserved;
};

/* DC.tc bit fields */
#define RISCV_IOMMU_DC_TC_V        (1ULL << 0)   /* Valid                */
#define RISCV_IOMMU_DC_TC_EN_ATS   (1ULL << 1)   /* ATS                 */
#define RISCV_IOMMU_DC_TC_DTF      (1ULL << 4)   /* Disable Tx Faults   */

/* DC.iohgatp fields */
#define RISCV_IOMMU_DC_IOHGATP_MODE_OFF   60
#define RISCV_IOMMU_DC_IOHGATP_GSCID_OFF  44
#define RISCV_IOMMU_DC_IOHGATP_GSCID_MSK  (0xFFFFULL << 44)
#define RISCV_IOMMU_DC_IOHGATP_PPN_MSK    0xFFFFFFFFFFFULL  /* bits [43:0] */

#define RISCV_IOMMU_IOHGATP_BARE    0ULL
#define RISCV_IOMMU_IOHGATP_SV39X4  8ULL
#define RISCV_IOMMU_IOHGATP_SV48X4  9ULL
#define RISCV_IOMMU_IOHGATP_SV57X4  10ULL

/* ═══════════════════════════════════════════════════════════════════════════
 * Command queue command format (128-bit: dword0 + dword1)
 *   dword0[6:0]  = opcode
 *   dword0[9:7]  = func3
 * ═══════════════════════════════════════════════════════════════════════════ */

/* IODIR.INVAL_DDT: opcode=3, func=0 → dword0 low7=0x03 */
#define RISCV_IOMMU_CMD_IODIR_INVAL_DDT_OPCODE  0x03
#define RISCV_IOMMU_CMD_IODIR_DV                 (1ULL << 33)
#define RISCV_IOMMU_CMD_IODIR_DID_OFF            40

/* IOFENCE.C: opcode=2, func=0 → dword0 low7=0x02 */
#define RISCV_IOMMU_CMD_IOFENCE_OPCODE           0x02

/* IOTINVAL.GVMA: opcode=1, func=1 → dword0 = 0x81 */
#define RISCV_IOMMU_CMD_IOTINVAL_GVMA            0x81
#define RISCV_IOMMU_CMD_IOTINVAL_GV              (1ULL << 33)
#define RISCV_IOMMU_CMD_IOTINVAL_GSCID_OFF       44

/* ═══════════════════════════════════════════════════════════════════════════
 * Fault queue record (32 bytes)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct riscv_iommu_fq_record {
    uint64_t hdr;
    uint64_t _reserved;
    uint64_t iotval;
    uint64_t iotval2;
};

#define RISCV_IOMMU_FQ_HDR_CAUSE_MSK  0xFFFULL
#define RISCV_IOMMU_FQ_HDR_DID_OFF    40
#define RISCV_IOMMU_FQ_HDR_DID_MSK    (0xFFFFFFULL << 40)

/* ═══════════════════════════════════════════════════════════════════════════
 * Per-VM IOMMU architecture state
 * Stored in struct vm → struct vm_io → struct io_prot → iommu_vm_arch
 * ═══════════════════════════════════════════════════════════════════════════ */

struct iommu_vm_arch {
    ssize_t gscid;  /* Guest Soft-Context ID (typically == vm_id) */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Maximum device count for 1LVL DDT mode
 * One page (4 KiB) / 64 bytes per DC = 64 Device Contexts
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RISCV_IOMMU_DDT_1LVL_MAX_DEVS  (PAGE_SIZE / sizeof(struct riscv_iommu_dc))

#endif /* __IOMMU_ARCH_H__ */
