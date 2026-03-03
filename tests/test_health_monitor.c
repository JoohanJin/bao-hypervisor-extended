/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Unit tests for the VM Health Monitor (heartbeat-based liveness detection).
 *
 * Build & run:
 *   cd tests && make test_health_monitor && ./test_health_monitor
 *
 * All tests run on the host (macOS/Linux) using the HEALTH_MONITOR_TEST
 * compile-time mock that replaces rdtime() with a controllable variable.
 */

#ifndef HEALTH_MONITOR_TEST
#define HEALTH_MONITOR_TEST
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ------------------------------------------------------------------ */
/*  Mock infrastructure                                                */
/* ------------------------------------------------------------------ */

/* Mock time source — controlled by tests */
uint64_t health_mock_time = 0;

/* Minimal struct vm stub — just enough for health monitor */
typedef unsigned int vmid_t;

struct vm_health_stub; /* forward */

#include "../src/core/inc/health_monitor.h"

struct vm {
    vmid_t id;
    struct vm_health health;
};

/* Include the implementation directly (with test mock active) */
/* We redefine the functions inline since we can't link vm.h, etc. */

/*
 * Re-implement health_monitor.c logic here under the test mock.
 * (The real .c file has #ifndef HEALTH_MONITOR_TEST guards on printk.)
 */
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
    if (vm->health.status == VM_NOT_MONITORED) return;

    vm->health.last_heartbeat = health_rdtime();
    vm->health.missed_count = 0;

    if (vm->health.status != VM_HEALTHY) {
        vm->health.status = VM_HEALTHY;
    }
}

void health_monitor_check(struct vm *vm)
{
    if (vm->health.status == VM_NOT_MONITORED) return;

    uint64_t now = health_rdtime();
    uint64_t elapsed = now - vm->health.last_heartbeat;

    if (elapsed <= vm->health.heartbeat_timeout) return;

    vm->health.missed_count++;

    if (vm->health.missed_count >= vm->health.max_missed) {
        vm->health.status = VM_UNHEALTHY;
    } else {
        vm->health.status = VM_SUSPECT;
    }
}

/* ------------------------------------------------------------------ */
/*  Test counters                                                      */
/* ------------------------------------------------------------------ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    static void name(void); \
    static void name(void)

#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  [%2d] %-55s ", tests_run, #name); \
    fflush(stdout); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: %s == %lld, expected %lld\n", \
               __FILE__, __LINE__, #a, (long long)(a), (long long)(b)); \
        exit(1); \
    } \
} while (0)

#define ASSERT_NEQ(a, b) do { \
    if ((a) == (b)) { \
        printf("FAIL\n    %s:%d: %s == %lld, should differ from %lld\n", \
               __FILE__, __LINE__, #a, (long long)(a), (long long)(b)); \
        exit(1); \
    } \
} while (0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s is false\n", \
               __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/*  Helper: create a VM with standard health config                    */
/* ------------------------------------------------------------------ */

static struct vm make_monitored_vm(vmid_t id, uint64_t timeout_ms, uint32_t max_missed)
{
    struct vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.id = id;

    struct vm_health_config cfg = {
        .enabled = true,
        .heartbeat_period_ms = timeout_ms,
        .timeout_ms = timeout_ms,
        .max_missed = max_missed,
    };

    health_monitor_init(&vm, &cfg);
    return vm;
}

static struct vm make_unmonitored_vm(vmid_t id)
{
    struct vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.id = id;

    struct vm_health_config cfg = { .enabled = false };
    health_monitor_init(&vm, &cfg);
    return vm;
}

/* ================================================================== */
/*  TEST CASES                                                         */
/* ================================================================== */

/* --- Initialization tests --- */

TEST(test_init_monitored_sets_healthy) {
    health_mock_time = 1000;
    struct vm vm = make_monitored_vm(0, 5000, 3);
    ASSERT_EQ(vm.health.status, VM_HEALTHY);
    ASSERT_EQ(vm.health.last_heartbeat, 1000);
    ASSERT_EQ(vm.health.missed_count, 0);
    ASSERT_EQ(vm.health.max_missed, 3);
    ASSERT_TRUE(vm.health.heartbeat_timeout > 0);
}

TEST(test_init_unmonitored_sets_not_monitored) {
    struct vm vm = make_unmonitored_vm(1);
    ASSERT_EQ(vm.health.status, VM_NOT_MONITORED);
    ASSERT_EQ(vm.health.max_missed, 0);
    ASSERT_EQ(vm.health.heartbeat_timeout, 0);
}

TEST(test_init_null_config_sets_not_monitored) {
    struct vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.id = 2;
    health_monitor_init(&vm, NULL);
    ASSERT_EQ(vm.health.status, VM_NOT_MONITORED);
}

TEST(test_init_default_max_missed) {
    health_mock_time = 0;
    struct vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.id = 3;
    struct vm_health_config cfg = {
        .enabled = true,
        .timeout_ms = 1000,
        .max_missed = 0,   /* should use default */
    };
    health_monitor_init(&vm, &cfg);
    ASSERT_EQ(vm.health.max_missed, HEALTH_DEFAULT_MAX_MISSED);
}

TEST(test_init_default_timeout) {
    health_mock_time = 0;
    struct vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.id = 4;
    struct vm_health_config cfg = {
        .enabled = true,
        .timeout_ms = 0,   /* should use default */
        .max_missed = 5,
    };
    health_monitor_init(&vm, &cfg);
    ASSERT_EQ(vm.health.heartbeat_timeout,
              HEALTH_MS_TO_TICKS(HEALTH_DEFAULT_TIMEOUT_MS));
}

/* --- Heartbeat tests --- */

TEST(test_heartbeat_updates_timestamp) {
    health_mock_time = 1000;
    struct vm vm = make_monitored_vm(0, 5000, 3);

    health_mock_time = 50000;
    health_monitor_heartbeat(&vm);

    ASSERT_EQ(vm.health.last_heartbeat, 50000);
    ASSERT_EQ(vm.health.status, VM_HEALTHY);
}

TEST(test_heartbeat_resets_missed_count) {
    health_mock_time = 0;
    struct vm vm = make_monitored_vm(0, 100, 5);

    /* Force some missed checks */
    vm.health.missed_count = 3;
    vm.health.status = VM_SUSPECT;

    health_mock_time = 999999;
    health_monitor_heartbeat(&vm);

    ASSERT_EQ(vm.health.missed_count, 0);
    ASSERT_EQ(vm.health.status, VM_HEALTHY);
}

TEST(test_heartbeat_on_unmonitored_is_noop) {
    struct vm vm = make_unmonitored_vm(0);
    health_mock_time = 5000;
    health_monitor_heartbeat(&vm);
    /* Should remain NOT_MONITORED */
    ASSERT_EQ(vm.health.status, VM_NOT_MONITORED);
}

TEST(test_heartbeat_recovers_from_unhealthy) {
    health_mock_time = 0;
    struct vm vm = make_monitored_vm(0, 100, 2);

    vm.health.status = VM_UNHEALTHY;
    vm.health.missed_count = 5;

    health_mock_time = 999999;
    health_monitor_heartbeat(&vm);

    ASSERT_EQ(vm.health.status, VM_HEALTHY);
    ASSERT_EQ(vm.health.missed_count, 0);
}

/* --- Health check tests --- */

TEST(test_check_within_timeout_stays_healthy) {
    health_mock_time = 0;
    struct vm vm = make_monitored_vm(0, 5000, 3);

    /* Advance time but still within timeout */
    health_mock_time = HEALTH_MS_TO_TICKS(4999);
    health_monitor_check(&vm);

    ASSERT_EQ(vm.health.status, VM_HEALTHY);
    ASSERT_EQ(vm.health.missed_count, 0);
}

TEST(test_check_at_exactly_timeout_stays_healthy) {
    health_mock_time = 0;
    struct vm vm = make_monitored_vm(0, 5000, 3);

    /* Exactly at timeout boundary (elapsed == timeout => not overdue) */
    health_mock_time = HEALTH_MS_TO_TICKS(5000);
    health_monitor_check(&vm);

    ASSERT_EQ(vm.health.status, VM_HEALTHY);
    ASSERT_EQ(vm.health.missed_count, 0);
}

TEST(test_check_past_timeout_becomes_suspect) {
    health_mock_time = 0;
    struct vm vm = make_monitored_vm(0, 5000, 3);

    /* Just past timeout */
    health_mock_time = HEALTH_MS_TO_TICKS(5001);
    health_monitor_check(&vm);

    ASSERT_EQ(vm.health.status, VM_SUSPECT);
    ASSERT_EQ(vm.health.missed_count, 1);
}

TEST(test_check_repeated_misses_become_unhealthy) {
    health_mock_time = 0;
    struct vm vm = make_monitored_vm(0, 1000, 3);

    /* Miss 1 */
    health_mock_time = HEALTH_MS_TO_TICKS(1001);
    health_monitor_check(&vm);
    ASSERT_EQ(vm.health.status, VM_SUSPECT);
    ASSERT_EQ(vm.health.missed_count, 1);

    /* Miss 2 */
    health_monitor_check(&vm);
    ASSERT_EQ(vm.health.status, VM_SUSPECT);
    ASSERT_EQ(vm.health.missed_count, 2);

    /* Miss 3 => UNHEALTHY */
    health_monitor_check(&vm);
    ASSERT_EQ(vm.health.status, VM_UNHEALTHY);
    ASSERT_EQ(vm.health.missed_count, 3);
}

TEST(test_check_stays_unhealthy_on_further_misses) {
    health_mock_time = 0;
    struct vm vm = make_monitored_vm(0, 1000, 2);

    health_mock_time = HEALTH_MS_TO_TICKS(1001);

    /* Two misses = UNHEALTHY */
    health_monitor_check(&vm);
    health_monitor_check(&vm);
    ASSERT_EQ(vm.health.status, VM_UNHEALTHY);

    /* Further checks stay UNHEALTHY */
    health_monitor_check(&vm);
    health_monitor_check(&vm);
    ASSERT_EQ(vm.health.status, VM_UNHEALTHY);
    ASSERT_EQ(vm.health.missed_count, 4);
}

TEST(test_check_on_unmonitored_is_noop) {
    struct vm vm = make_unmonitored_vm(0);
    health_mock_time = 99999999;
    health_monitor_check(&vm);
    ASSERT_EQ(vm.health.status, VM_NOT_MONITORED);
}

/* --- Full lifecycle tests --- */

TEST(test_lifecycle_healthy_suspect_recover) {
    health_mock_time = 0;
    struct vm vm = make_monitored_vm(0, 1000, 5);

    /* On-time heartbeat */
    health_mock_time = HEALTH_MS_TO_TICKS(500);
    health_monitor_heartbeat(&vm);
    health_monitor_check(&vm);
    ASSERT_EQ(vm.health.status, VM_HEALTHY);

    /* Miss deadline */
    health_mock_time = HEALTH_MS_TO_TICKS(500 + 1001);
    health_monitor_check(&vm);
    ASSERT_EQ(vm.health.status, VM_SUSPECT);
    ASSERT_EQ(vm.health.missed_count, 1);

    /* Recover with heartbeat */
    health_monitor_heartbeat(&vm);
    ASSERT_EQ(vm.health.status, VM_HEALTHY);
    ASSERT_EQ(vm.health.missed_count, 0);

    /* On-time check after recovery */
    health_mock_time += HEALTH_MS_TO_TICKS(500);
    health_monitor_check(&vm);
    ASSERT_EQ(vm.health.status, VM_HEALTHY);
}

TEST(test_lifecycle_healthy_to_unhealthy_to_recover) {
    health_mock_time = 0;
    struct vm vm = make_monitored_vm(0, 500, 2);

    /* Miss twice -> UNHEALTHY */
    health_mock_time = HEALTH_MS_TO_TICKS(501);
    health_monitor_check(&vm);
    ASSERT_EQ(vm.health.status, VM_SUSPECT);

    health_monitor_check(&vm);
    ASSERT_EQ(vm.health.status, VM_UNHEALTHY);

    /* Guest recovers */
    health_mock_time = HEALTH_MS_TO_TICKS(10000);
    health_monitor_heartbeat(&vm);
    ASSERT_EQ(vm.health.status, VM_HEALTHY);
    ASSERT_EQ(vm.health.missed_count, 0);
    ASSERT_EQ(vm.health.last_heartbeat, HEALTH_MS_TO_TICKS(10000));
}

/* --- Timer conversion tests --- */

TEST(test_ms_to_ticks_conversion) {
    ASSERT_EQ(HEALTH_MS_TO_TICKS(1000), HEALTH_TIMER_FREQ);
    ASSERT_EQ(HEALTH_MS_TO_TICKS(500), HEALTH_TIMER_FREQ / 2);
    ASSERT_EQ(HEALTH_MS_TO_TICKS(0), 0);
    ASSERT_EQ(HEALTH_MS_TO_TICKS(1), HEALTH_TIMER_FREQ / 1000);
}

/* --- Status string tests --- */

TEST(test_health_status_str) {
    ASSERT_TRUE(strcmp(health_status_str(VM_HEALTHY), "HEALTHY") == 0);
    ASSERT_TRUE(strcmp(health_status_str(VM_SUSPECT), "SUSPECT") == 0);
    ASSERT_TRUE(strcmp(health_status_str(VM_UNHEALTHY), "UNHEALTHY") == 0);
    ASSERT_TRUE(strcmp(health_status_str(VM_NOT_MONITORED), "NOT_MONITORED") == 0);
    ASSERT_TRUE(strcmp(health_status_str(99), "UNKNOWN") == 0);
}

/* --- Edge case: max_missed = 1 --- */

TEST(test_single_miss_becomes_unhealthy) {
    health_mock_time = 0;
    struct vm vm = make_monitored_vm(0, 1000, 1);

    health_mock_time = HEALTH_MS_TO_TICKS(1001);
    health_monitor_check(&vm);

    /* With max_missed=1, first miss goes straight to UNHEALTHY */
    ASSERT_EQ(vm.health.status, VM_UNHEALTHY);
    ASSERT_EQ(vm.health.missed_count, 1);
}

/* --- Edge case: multiple VMs independent --- */

TEST(test_two_vms_independent_health) {
    health_mock_time = 0;
    struct vm vm0 = make_monitored_vm(0, 1000, 3);
    struct vm vm1 = make_monitored_vm(1, 2000, 3);

    /* Both start healthy */
    ASSERT_EQ(vm0.health.status, VM_HEALTHY);
    ASSERT_EQ(vm1.health.status, VM_HEALTHY);

    /* VM0 misses deadline, VM1 is still within timeout */
    health_mock_time = HEALTH_MS_TO_TICKS(1500);
    health_monitor_check(&vm0);
    health_monitor_check(&vm1);

    ASSERT_EQ(vm0.health.status, VM_SUSPECT);
    ASSERT_EQ(vm1.health.status, VM_HEALTHY);

    /* VM1 sends heartbeat */
    health_monitor_heartbeat(&vm1);
    ASSERT_EQ(vm1.health.last_heartbeat, HEALTH_MS_TO_TICKS(1500));

    /* Both miss deadline at much later time */
    health_mock_time = HEALTH_MS_TO_TICKS(50000);
    health_monitor_check(&vm0);
    health_monitor_check(&vm0);
    health_monitor_check(&vm1);

    ASSERT_EQ(vm0.health.status, VM_UNHEALTHY); /* missed_count >= 3 */
    ASSERT_EQ(vm1.health.status, VM_SUSPECT);    /* first miss for vm1 */
}

/* ================================================================== */
/*  Main test runner                                                   */
/* ================================================================== */

int main(void)
{
    printf("\n=== Health Monitor Unit Tests ===\n\n");

    /* Initialization */
    RUN_TEST(test_init_monitored_sets_healthy);
    RUN_TEST(test_init_unmonitored_sets_not_monitored);
    RUN_TEST(test_init_null_config_sets_not_monitored);
    RUN_TEST(test_init_default_max_missed);
    RUN_TEST(test_init_default_timeout);

    /* Heartbeat */
    RUN_TEST(test_heartbeat_updates_timestamp);
    RUN_TEST(test_heartbeat_resets_missed_count);
    RUN_TEST(test_heartbeat_on_unmonitored_is_noop);
    RUN_TEST(test_heartbeat_recovers_from_unhealthy);

    /* Health check */
    RUN_TEST(test_check_within_timeout_stays_healthy);
    RUN_TEST(test_check_at_exactly_timeout_stays_healthy);
    RUN_TEST(test_check_past_timeout_becomes_suspect);
    RUN_TEST(test_check_repeated_misses_become_unhealthy);
    RUN_TEST(test_check_stays_unhealthy_on_further_misses);
    RUN_TEST(test_check_on_unmonitored_is_noop);

    /* Full lifecycle */
    RUN_TEST(test_lifecycle_healthy_suspect_recover);
    RUN_TEST(test_lifecycle_healthy_to_unhealthy_to_recover);

    /* Conversions & strings */
    RUN_TEST(test_ms_to_ticks_conversion);
    RUN_TEST(test_health_status_str);

    /* Edge cases */
    RUN_TEST(test_single_miss_becomes_unhealthy);
    RUN_TEST(test_two_vms_independent_health);

    printf("\n=== %d / %d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
