/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * VM Auto-Recovery — Restart failed VMs without affecting other VMs.
 */

#ifndef VM_RECOVERY_TEST
#include <vm_recovery.h>
#include <vm.h>
#include <cpu.h>
#include <config.h>
#include <health_monitor.h>
#include <printk.h>

static void vm_recovery_msg_handler(uint32_t event, uint64_t data);
CPU_MSG_HANDLER(vm_recovery_msg_handler, VM_RECOVERY_MSG_ID);
#else
/* Test build: headers and stubs provided by the test file */
#endif

void vm_recovery_start(struct vm *vm)
{
    const struct vm_health_config *cfg = &vm->config->health;

    if (!cfg->auto_recover) {
        return;
    }

    if (cfg->max_recoveries > 0 &&
        vm->health.recovery_count >= cfg->max_recoveries) {
#ifndef VM_RECOVERY_TEST
        INFO("VM %d exceeded max recoveries (%d), not recovering",
             vm->id, cfg->max_recoveries);
#endif
        return;
    }

    if (vm->health.status == VM_RECOVERING) {
        return;  /* already recovering, prevent re-entrance */
    }

    vm->health.status = VM_RECOVERING;
    vm->health.recovery_count++;

#ifndef VM_RECOVERY_TEST
    INFO("[RECOVERY] VM %d starting recovery (#%d)",
         vm->id, vm->health.recovery_count);
#endif

    /* Send VM_RESET IPI to all remote vCPUs of this VM */
    struct cpu_msg msg = {
        .handler = VM_RECOVERY_MSG_ID,
        .event = VM_RESET,
        .data = 0,
    };
    vm_msg_broadcast(vm, &msg);

    /* Execute recovery immediately on master CPU.
     * The interrupt return path (exceptions.S) goes directly to
     * vcpu_arch_entry without passing through vcpu_arch_run(),
     * so we cannot defer recovery — we must do it here.
     * vm_recovery_execute() does not return (calls vcpu_run()). */
    if (cpu()->id == vm->master) {
        vm_recovery_execute(vm);
        /* does not return */
    }
}

void vm_recovery_execute(struct vm *vm)
{
    struct vcpu *vcpu = cpu()->vcpu;

    /* 1. Barrier — wait for all remote vCPUs to stop */
    cpu_sync_barrier(&vm->sync);

    /* 2. Re-install guest image (master only) */
    vm_reinstall_image(vm);

    /* 3. Barrier — image is ready */
    cpu_sync_barrier(&vm->sync);

    /* 4. Reset our own vCPU */
    vcpu_arch_reset(vcpu, vm->config->entry);
    vcpu->arch.sbi_ctx.state = STARTED;

    /* 5. Re-init health state */
    health_monitor_init(vm, &vm->config->health);

    /* 6. Final barrier — all vCPUs reset */
    cpu_sync_barrier(&vm->sync);

#ifndef VM_RECOVERY_TEST
    INFO("[RECOVERY] VM %d recovered successfully", vm->id);
#endif

    /* 7. Resume execution — does not return */
    vcpu_run(vcpu);
}

#ifndef VM_RECOVERY_TEST
static void vm_recovery_msg_handler(uint32_t event, uint64_t data)
{
    (void)data;
    if (event != VM_RESET) return;

    struct vcpu *vcpu = cpu()->vcpu;
    struct vm *vm = vcpu->vm;

    vcpu->active = false;

    /* 1. Barrier — signal we've stopped */
    cpu_sync_barrier(&vm->sync);

    /* 2. Barrier — wait for master to re-install image */
    cpu_sync_barrier(&vm->sync);

    /* 3. Reset our vCPU */
    vcpu_arch_reset(vcpu, vm->config->entry);
    vcpu->arch.sbi_ctx.state = vcpu->id == 0 ? STARTED : STOPPED;

    /* 4. Final barrier */
    cpu_sync_barrier(&vm->sync);

    /* 5. Resume */
    vcpu_run(vcpu);
}
#endif
