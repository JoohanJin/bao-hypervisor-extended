/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * VM Health Monitor — Heartbeat-Based Liveness Detection
 *
 * Implements a server (hypervisor) / client (guest VM) health monitoring
 * model. Guest VMs periodically send HC_HEARTBEAT hypercalls. The
 * hypervisor checks elapsed time against per-VM deadlines on each timer
 * interrupt and transitions VMs through health states:
 *
 *   VM_HEALTHY  →  VM_SUSPECT  →  VM_UNHEALTHY
 *
 * Recovery actions are logged to the hypervisor console (Phase 1).
 * Future phases will add restart, IPC notification, and resource
 * reallocation.
 */

#ifndef __HEALTH_MONITOR_H__
#define __HEALTH_MONITOR_H__

#ifdef HEALTH_MONITOR_TEST
/* Host-side unit test build: use standard headers */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#else
#include <bao.h>
#endif

/* ------------------------------------------------------------------ */
/*  Health states                                                      */
/* ------------------------------------------------------------------ */

enum vm_health_status {
    VM_HEALTHY       = 0,  /**< Heartbeats arriving on time           */
    VM_SUSPECT       = 1,  /**< 1+ missed, within grace period        */
    VM_UNHEALTHY     = 2,  /**< Exceeded max missed heartbeats        */
    VM_NOT_MONITORED = 3,  /**< Heartbeat not configured for this VM  */
};

/* ------------------------------------------------------------------ */
/*  Per-VM health state  (embedded in struct vm)                       */
/* ------------------------------------------------------------------ */

struct vm_health {
    enum vm_health_status status;
    uint64_t last_heartbeat;      /**< rdtime() at last heartbeat      */
    uint32_t missed_count;        /**< Consecutive missed checks       */
    uint32_t max_missed;          /**< Threshold before UNHEALTHY      */
    uint64_t heartbeat_timeout;   /**< Ticks before a check is "missed"*/
};

/* ------------------------------------------------------------------ */
/*  Per-VM health configuration  (embedded in struct vm_config)        */
/* ------------------------------------------------------------------ */

struct vm_health_config {
    bool     enabled;              /**< false → VM_NOT_MONITORED       */
    uint64_t heartbeat_period_ms;  /**< Expected guest heartbeat (ms)  */
    uint64_t timeout_ms;           /**< Deadline before "missed" (ms)  */
    uint32_t max_missed;           /**< Missed checks → UNHEALTHY      */
};

/* ------------------------------------------------------------------ */
/*  Timer constants                                                    */
/* ------------------------------------------------------------------ */

/**
 * Platform timer frequency (ticks per second).
 * QEMU virt: 10 MHz.  Override at compile time for other platforms.
 */
#ifndef HEALTH_TIMER_FREQ
#define HEALTH_TIMER_FREQ           10000000ULL
#endif

/** Convert milliseconds to timer ticks */
#define HEALTH_MS_TO_TICKS(ms) \
    ((uint64_t)(ms) * HEALTH_TIMER_FREQ / 1000ULL)

/** Default values (used when config fields are zero) */
#define HEALTH_DEFAULT_TIMEOUT_MS   5000   /* 5 seconds  */
#define HEALTH_DEFAULT_MAX_MISSED   3

/**
 * Watchdog interval: if the guest stops programming its timer, the
 * hypervisor re-arms a fallback timer at this interval so health
 * checks keep running.
 */
#define HEALTH_WATCHDOG_TICKS       HEALTH_MS_TO_TICKS(2000)

/* ------------------------------------------------------------------ */
/*  Monotonic time source                                              */
/* ------------------------------------------------------------------ */

#ifdef HEALTH_MONITOR_TEST
extern uint64_t health_mock_time;
static inline uint64_t health_rdtime(void)
{
    return health_mock_time;
}
#else
/**
 * Read the RISC-V time CSR (monotonic wall-clock counter).
 * This is accessible from HS-mode when mcounteren.TM = 1 (set by
 * OpenSBI / M-mode firmware).
 */
static inline uint64_t health_rdtime(void)
{
    uint64_t val;
    __asm__ volatile("rdtime %0" : "=r"(val));
    return val;
}
#endif

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

struct vm;   /* forward declaration */

/**
 * Initialize a VM's health monitoring state from its configuration.
 * Called once during vm_master_init().
 */
void health_monitor_init(struct vm *vm, const struct vm_health_config *cfg);

/**
 * Record a heartbeat from the guest (called from HC_HEARTBEAT handler).
 * Resets missed counter and transitions status back to HEALTHY.
 */
void health_monitor_heartbeat(struct vm *vm);

/**
 * Check a VM's liveness (called from sbi_timer_irq_handler).
 * Compares elapsed time since last heartbeat against the configured
 * timeout and updates the health status accordingly.
 */
void health_monitor_check(struct vm *vm);

/**
 * Return a human-readable label for a health status value.
 */
static inline const char *health_status_str(enum vm_health_status s)
{
    switch (s) {
        case VM_HEALTHY:        return "HEALTHY";
        case VM_SUSPECT:        return "SUSPECT";
        case VM_UNHEALTHY:      return "UNHEALTHY";
        case VM_NOT_MONITORED:  return "NOT_MONITORED";
        default:                return "UNKNOWN";
    }
}

#endif /* __HEALTH_MONITOR_H__ */
