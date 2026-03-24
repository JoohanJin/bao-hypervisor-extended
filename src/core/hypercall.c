/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

#include <hypercall.h>
#include <cpu.h>
#include <vm.h>
#include <ipc.h>
#include <health_monitor.h>
#include <arch/vplic.h>
#include <irq_rate_limit.h>
#include <mem.h>
#include <arch/iommu.h>
#include <fences.h>

long int hypercall(unsigned long id) {
    long int ret = -HC_E_INVAL_ID;

    unsigned long ipc_id = vcpu_readreg(cpu()->vcpu, HYPCALL_ARG_REG(0));
    unsigned long arg1   = vcpu_readreg(cpu()->vcpu, HYPCALL_ARG_REG(1));
    unsigned long arg2   = vcpu_readreg(cpu()->vcpu, HYPCALL_ARG_REG(2));

    switch(id){
        case HC_IPC:
            ret = ipc_hypercall(ipc_id, arg1, arg2);
        break;
        case HC_HEARTBEAT:
#ifndef BENCH_NO_HEALTH_MONITOR
            health_monitor_heartbeat(cpu()->vcpu->vm);
#endif
            ret = HC_E_SUCCESS;
        break;
        case HC_PLIC_INJECT: {
            static uint64_t inject_total = 0;
            static uint64_t inject_allowed = 0;
            static uint64_t inject_deferred = 0;
            static uint64_t inject_dropped = 0;
            /* Test rate limiter directly — bypass vplic pending check */
#ifndef BENCH_NO_RATE_LIMIT
            irqid_t irq_id = (irqid_t)ipc_id;
            struct vplic *vplic = &cpu()->vcpu->vm->arch.vplic;
            enum irq_rl_action action = irq_bucket_consume(&vplic->buckets[irq_id]);
            if (action == IRQ_RL_ALLOW) inject_allowed++;
            else if (action == IRQ_RL_DEFERRED) inject_deferred++;
            else inject_dropped++;
#else
            inject_allowed++;
#endif

            inject_total++;
            if ((inject_total % 100000) == 0) {
                printk("[BENCH:RL] total=%lu allowed=%lu deferred=%lu dropped=%lu\n",
                       (unsigned long)inject_total,
                       (unsigned long)inject_allowed,
                       (unsigned long)inject_deferred,
                       (unsigned long)inject_dropped);
            }
            ret = HC_E_SUCCESS;
            break;
        }
        case HC_DMA_WRITE: {
            paddr_t target_pa = (paddr_t)ipc_id;
            uint32_t write_val = (uint32_t)arg1;

            if (iommu_active()) {
                INFO("[BENCH:IOMMU:DMA_BLOCKED] vm%d write 0x%x to PA 0x%lx",
                     cpu()->vcpu->vm->id, write_val, target_pa);
                ret = HC_E_FAILURE;
            } else {
                paddr_t page_pa = target_pa & ~((paddr_t)PAGE_SIZE - 1);
                size_t  offset  = target_pa & (PAGE_SIZE - 1);
                vaddr_t va = mem_alloc_map_dev(&cpu()->as, SEC_HYP_GLOBAL,
                                               INVALID_VA, page_pa, 1);
                if (va) {
                    *(volatile uint32_t *)(va + offset) = write_val;
                    fence_sync_write();
                    mem_unmap(&cpu()->as, va, 1, false);
                    INFO("[BENCH:DMA:WRITE_OK] vm%d wrote 0x%x to PA 0x%lx",
                         cpu()->vcpu->vm->id, write_val, target_pa);
                    ret = HC_E_SUCCESS;
                } else {
                    ret = HC_E_FAILURE;
                }
            }
            break;
        }
        default:
            WARNING("Unknown hypercall id %d", id);
    }

    return ret;
}
