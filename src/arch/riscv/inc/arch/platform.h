/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

#ifndef __ARCH_PLATFORM_H__
#define __ARCH_PLATFORM_H__

#include <bao.h>

struct arch_platform {
    paddr_t plic_base;

    /**
     * RISC-V IOMMU for DMA isolation.
     * Set base = 0 if no IOMMU is present on this platform.
     */
    struct {
        paddr_t base;   /* MMIO base address (e.g. 0x3010000 on QEMU virt) */
    } iommu;
};

#endif /* __ARCH_PLATFORM_H__ */
