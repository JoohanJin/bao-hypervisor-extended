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
        default:
            WARNING("Unknown hypercall id %d", id);
    }

    return ret;
}
