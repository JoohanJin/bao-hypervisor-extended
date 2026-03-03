/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * VM Health Monitor — Core Logic
 *
 * Implements heartbeat recording and periodic liveness checks.
 * All functions are safe to call from interrupt context (no blocking).
 */

#include <health_monitor.h>

#ifndef HEALTH_MONITOR_TEST
#include <vm.h>
#include <printk.h>
#endif

void health_monitor_init(struct vm *vm, const struct vm_health_config *cfg)
{
    if (!cfg || !cfg->enabled) {
        vm->health.status = VM_NOT_MONITORED;
        vm->health.last_heartbeat = 0;
        vm->health.missed_count = 0;
        vm->health.max_missed = 0;
        vm->health.heartbeat_timeout = 0;
        return;
    }

    vm->health.status = VM_HEALTHY;
    vm->health.last_heartbeat = health_rdtime();
    vm->health.missed_count = 0;

    vm->health.max_missed = cfg->max_missed > 0
        ? cfg->max_missed
        : HEALTH_DEFAULT_MAX_MISSED;

    vm->health.heartbeat_timeout = cfg->timeout_ms > 0
        ? HEALTH_MS_TO_TICKS(cfg->timeout_ms)
        : HEALTH_MS_TO_TICKS(HEALTH_DEFAULT_TIMEOUT_MS);
}

void health_monitor_heartbeat(struct vm *vm)
{
    if (vm->health.status == VM_NOT_MONITORED) {
        return;
    }

    vm->health.last_heartbeat = health_rdtime();
    vm->health.missed_count = 0;

    if (vm->health.status != VM_HEALTHY) {
#ifndef HEALTH_MONITOR_TEST
        printk("HEALTH: VM %d recovered -> HEALTHY\n", vm->id);
#endif
        vm->health.status = VM_HEALTHY;
    }
}

void health_monitor_check(struct vm *vm)
{
    if (vm->health.status == VM_NOT_MONITORED) {
        return;
    }

    uint64_t now = health_rdtime();
    uint64_t elapsed = now - vm->health.last_heartbeat;

    if (elapsed <= vm->health.heartbeat_timeout) {
        /* Heartbeat is on time — nothing to do */
        return;
    }

    /* Heartbeat overdue */
    vm->health.missed_count++;

    enum vm_health_status prev = vm->health.status;

    if (vm->health.missed_count >= vm->health.max_missed) {
        vm->health.status = VM_UNHEALTHY;
        if (prev != VM_UNHEALTHY) {
#ifndef HEALTH_MONITOR_TEST
            printk("HEALTH: VM %d UNHEALTHY (%d missed heartbeats)\n",
                   vm->id, vm->health.missed_count);
#endif
        }
    } else {
        vm->health.status = VM_SUSPECT;
        if (prev == VM_HEALTHY) {
#ifndef HEALTH_MONITOR_TEST
            printk("HEALTH: VM %d SUSPECT (%d/%d missed)\n",
                   vm->id, vm->health.missed_count, vm->health.max_missed);
#endif
        }
    }
}
