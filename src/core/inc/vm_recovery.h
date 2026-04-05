/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * VM Auto-Recovery — Restart failed VMs without affecting other VMs.
 *
 * When the health monitor detects VM_UNHEALTHY, the hypervisor can
 * automatically restart that VM by re-copying its guest image and
 * resetting all vCPU registers to the entry point.
 */

#ifndef __VM_RECOVERY_H__
#define __VM_RECOVERY_H__

#include <bao.h>

struct vm;  /* forward declaration */

/** IPI message handler ID for recovery coordination */
extern volatile const size_t VM_RECOVERY_MSG_ID;

/** Recovery IPI event types */
enum vm_recovery_event {
    VM_RESET = 0,
};

/**
 * Start auto-recovery for a failed VM.
 * Called from health_monitor_check() when VM transitions to UNHEALTHY.
 * Sets VM_RECOVERING state and sends VM_RESET IPI to remote vCPUs.
 */
void vm_recovery_start(struct vm *vm);

/**
 * Execute recovery on the master CPU.
 * Re-installs guest image, resets vCPUs, re-inits health monitor.
 * Called from the deferred recovery path in vcpu_arch_run().
 * Does not return — re-enters guest via vcpu_run().
 */
void vm_recovery_execute(struct vm *vm);

#endif /* __VM_RECOVERY_H__ */
