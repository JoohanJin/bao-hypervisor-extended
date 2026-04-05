/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Unit tests for VM Auto-Recovery (vm_recovery_start / vm_recovery_execute).
 *
 * Build & run:
 *   cd tests && make test_vm_recovery && ./test_vm_recovery
 *
 * Runs on the host (macOS/Linux) with mock stubs for hypervisor primitives.
 * Uses longjmp to handle "does not return" functions (vcpu_run).
 */

#ifndef VM_RECOVERY_TEST
#define VM_RECOVERY_TEST
#endif

#ifndef HEALTH_MONITOR_TEST
#define HEALTH_MONITOR_TEST
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

/* ------------------------------------------------------------------ */
/*  Mock time source for health monitor                                */
/* ------------------------------------------------------------------ */

uint64_t health_mock_time = 0;

/* ------------------------------------------------------------------ */
/*  Type stubs                                                         */
/* ------------------------------------------------------------------ */

typedef unsigned int vmid_t;
typedef unsigned int vcpuid_t;
typedef unsigned int cpuid_t;
typedef unsigned long vaddr_t;

/* Include health_monitor.h for enums, structs, and config */
#include "../src/core/inc/health_monitor.h"

/* SBI state constants (from real code) */
#define STARTED 1
#define STOPPED 0

/* cpu_msg stub — matches the real struct layout used by vm_recovery_start */
struct cpu_msg {
    size_t handler;
    unsigned int event;
    unsigned long data;
};

/* Minimal struct stubs */
struct vm_config {
    vaddr_t entry;
    struct vm_health_config health;
};

struct sbi_ctx {
    int state;
};

struct vcpu_arch {
    struct sbi_ctx sbi_ctx;
};

struct vcpu {
    vcpuid_t id;
    bool active;
    struct vm *vm;
    struct vcpu_arch arch;
};

struct vm {
    vmid_t id;
    const struct vm_config *config;
    cpuid_t master;
    struct vm_health health;
    int sync;  /* stub for cpu_synctoken */
};

/* ------------------------------------------------------------------ */
/*  Mock tracking                                                      */
/* ------------------------------------------------------------------ */

static struct {
    int broadcast_count;
    int reinstall_count;
    int arch_reset_count;
    int barrier_count;
    int vcpu_run_count;
    vaddr_t last_reset_entry;
    cpuid_t mock_cpu_id;
} mock;

static struct vcpu mock_vcpu;

struct cpu_state {
    cpuid_t id;
    struct vcpu *vcpu;
};

static struct cpu_state mock_cpu_state;

/* longjmp target: vcpu_run() and vm_recovery_execute() don't return */
static jmp_buf recovery_return;

static void reset_mocks(void)
{
    memset(&mock, 0, sizeof(mock));
    memset(&mock_vcpu, 0, sizeof(mock_vcpu));
    memset(&mock_cpu_state, 0, sizeof(mock_cpu_state));
    mock_cpu_state.vcpu = &mock_vcpu;
    health_mock_time = 0;
}

/* ------------------------------------------------------------------ */
/*  Mock functions                                                     */
/* ------------------------------------------------------------------ */

void vm_msg_broadcast(struct vm *vm, struct cpu_msg *msg)
{
    (void)vm;
    (void)msg;
    mock.broadcast_count++;
}

void vm_reinstall_image(struct vm *vm)
{
    (void)vm;
    mock.reinstall_count++;
}

void vcpu_arch_reset(struct vcpu *v, vaddr_t entry)
{
    (void)v;
    mock.arch_reset_count++;
    mock.last_reset_entry = entry;
}

void cpu_sync_barrier(void *token)
{
    (void)token;
    mock.barrier_count++;
}

void vcpu_run(struct vcpu *v)
{
    (void)v;
    mock.vcpu_run_count++;
    longjmp(recovery_return, 1);
}

struct cpu_state *cpu(void)
{
    mock_cpu_state.id = mock.mock_cpu_id;
    return &mock_cpu_state;
}

/* ------------------------------------------------------------------ */
/*  Re-implement health_monitor functions under test mock              */
/* ------------------------------------------------------------------ */

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

void health_monitor_check(struct vm *vm)
{
    if (vm->health.status == VM_NOT_MONITORED ||
        vm->health.status == VM_RECOVERING) {
        return;
    }

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

/* Forward declarations needed by vm_recovery.c */
enum vm_recovery_event {
    VM_RESET = 0,
};
volatile const size_t VM_RECOVERY_MSG_ID = 42;
void vm_recovery_execute(struct vm *vm);

/* Include vm_recovery.c implementation (guarded by VM_RECOVERY_TEST) */
#include "../src/core/vm_recovery.c"

/* ------------------------------------------------------------------ */
/*  Test harness macros                                                */
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

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s is false\n", \
               __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static struct vm_config make_recovery_config(vaddr_t entry, bool auto_recover,
                                              uint32_t max_recoveries)
{
    struct vm_config cfg = {
        .entry = entry,
        .health = {
            .enabled = true,
            .timeout_ms = 3000,
            .max_missed = 3,
            .auto_recover = auto_recover,
            .max_recoveries = max_recoveries,
        },
    };
    return cfg;
}

static struct vm make_test_vm(vmid_t id, const struct vm_config *cfg,
                               cpuid_t master)
{
    struct vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.id = id;
    vm.config = cfg;
    vm.master = master;
    vm.health.status = VM_UNHEALTHY;
    vm.health.missed_count = 5;
    vm.health.recovery_count = 0;
    return vm;
}

/* ================================================================== */
/*  TEST CASES — vm_recovery_start() guards                            */
/* ================================================================== */

TEST(test_start_disabled_auto_recover) {
    reset_mocks();
    struct vm_config cfg = make_recovery_config(0x80200000, false, 0);
    struct vm vm = make_test_vm(0, &cfg, 0);
    vm.health.status = VM_UNHEALTHY;

    vm_recovery_start(&vm);

    ASSERT_EQ(vm.health.status, VM_UNHEALTHY);  /* unchanged */
    ASSERT_EQ(mock.broadcast_count, 0);
    ASSERT_EQ(vm.health.recovery_count, 0);
}

TEST(test_start_max_recoveries_exceeded) {
    reset_mocks();
    struct vm_config cfg = make_recovery_config(0x80200000, true, 3);
    struct vm vm = make_test_vm(0, &cfg, 0);
    vm.health.recovery_count = 3;  /* already at limit */

    vm_recovery_start(&vm);

    ASSERT_EQ(vm.health.status, VM_UNHEALTHY);  /* unchanged */
    ASSERT_EQ(mock.broadcast_count, 0);
    ASSERT_EQ(vm.health.recovery_count, 3);     /* not incremented */
}

TEST(test_start_already_recovering) {
    reset_mocks();
    struct vm_config cfg = make_recovery_config(0x80200000, true, 5);
    struct vm vm = make_test_vm(0, &cfg, 0);
    vm.health.status = VM_RECOVERING;

    vm_recovery_start(&vm);

    ASSERT_EQ(vm.health.status, VM_RECOVERING);  /* unchanged */
    ASSERT_EQ(mock.broadcast_count, 0);
    ASSERT_EQ(vm.health.recovery_count, 0);      /* not incremented */
}

TEST(test_start_sets_recovering_state) {
    reset_mocks();
    mock.mock_cpu_id = 99;  /* non-master to avoid execute path */
    struct vm_config cfg = make_recovery_config(0x80200000, true, 5);
    struct vm vm = make_test_vm(0, &cfg, 0);
    vm.health.status = VM_UNHEALTHY;

    vm_recovery_start(&vm);

    ASSERT_EQ(vm.health.status, VM_RECOVERING);
}

TEST(test_start_increments_recovery_count) {
    reset_mocks();
    mock.mock_cpu_id = 99;  /* non-master */
    struct vm_config cfg = make_recovery_config(0x80200000, true, 10);
    struct vm vm = make_test_vm(0, &cfg, 0);

    vm.health.recovery_count = 0;
    vm.health.status = VM_UNHEALTHY;
    vm_recovery_start(&vm);
    ASSERT_EQ(vm.health.recovery_count, 1);

    /* Reset for second recovery */
    vm.health.status = VM_UNHEALTHY;
    vm_recovery_start(&vm);
    ASSERT_EQ(vm.health.recovery_count, 2);
}

TEST(test_start_broadcasts_reset_ipi) {
    reset_mocks();
    mock.mock_cpu_id = 99;  /* non-master */
    struct vm_config cfg = make_recovery_config(0x80200000, true, 5);
    struct vm vm = make_test_vm(0, &cfg, 0);
    vm.health.status = VM_UNHEALTHY;

    vm_recovery_start(&vm);

    ASSERT_EQ(mock.broadcast_count, 1);
}

TEST(test_start_calls_execute_on_master) {
    reset_mocks();
    mock.mock_cpu_id = 0;   /* matches vm.master */
    mock_vcpu.vm = NULL;    /* will be set up */
    struct vm_config cfg = make_recovery_config(0x80200000, true, 5);
    struct vm vm = make_test_vm(0, &cfg, 0);
    vm.health.status = VM_UNHEALTHY;
    mock_vcpu.vm = &vm;

    /* vm_recovery_execute calls vcpu_run which longjmps back */
    if (setjmp(recovery_return) == 0) {
        vm_recovery_start(&vm);
        /* Should not reach here — vcpu_run longjmps */
        ASSERT_TRUE(0);
    }

    /* Reached via longjmp from vcpu_run inside vm_recovery_execute */
    ASSERT_EQ(mock.vcpu_run_count, 1);
    ASSERT_EQ(mock.reinstall_count, 1);
    ASSERT_EQ(mock.broadcast_count, 1);
}

TEST(test_start_skips_execute_on_non_master) {
    reset_mocks();
    mock.mock_cpu_id = 3;   /* does NOT match vm.master=0 */
    struct vm_config cfg = make_recovery_config(0x80200000, true, 5);
    struct vm vm = make_test_vm(0, &cfg, 0);
    vm.health.status = VM_UNHEALTHY;

    vm_recovery_start(&vm);

    /* broadcast happens, but execute does NOT */
    ASSERT_EQ(mock.broadcast_count, 1);
    ASSERT_EQ(mock.reinstall_count, 0);
    ASSERT_EQ(mock.vcpu_run_count, 0);
}

TEST(test_start_unlimited_recoveries) {
    reset_mocks();
    mock.mock_cpu_id = 99;  /* non-master */
    struct vm_config cfg = make_recovery_config(0x80200000, true, 0);  /* 0 = unlimited */
    struct vm vm = make_test_vm(0, &cfg, 0);

    /* Simulate many recoveries */
    for (int i = 0; i < 100; i++) {
        vm.health.status = VM_UNHEALTHY;
        vm_recovery_start(&vm);
    }

    ASSERT_EQ(vm.health.recovery_count, 100);
    ASSERT_EQ(mock.broadcast_count, 100);
}

/* ================================================================== */
/*  TEST CASES — vm_recovery_execute()                                 */
/* ================================================================== */

TEST(test_execute_reinstalls_image) {
    reset_mocks();
    struct vm_config cfg = make_recovery_config(0x80200000, true, 5);
    struct vm vm = make_test_vm(0, &cfg, 0);
    mock_vcpu.vm = &vm;

    if (setjmp(recovery_return) == 0) {
        vm_recovery_execute(&vm);
        ASSERT_TRUE(0);
    }

    ASSERT_EQ(mock.reinstall_count, 1);
}

TEST(test_execute_resets_vcpu) {
    reset_mocks();
    struct vm_config cfg = make_recovery_config(0xDEAD0000, true, 5);
    struct vm vm = make_test_vm(0, &cfg, 0);
    mock_vcpu.vm = &vm;

    if (setjmp(recovery_return) == 0) {
        vm_recovery_execute(&vm);
        ASSERT_TRUE(0);
    }

    ASSERT_EQ(mock.arch_reset_count, 1);
    ASSERT_EQ(mock.last_reset_entry, 0xDEAD0000);
    ASSERT_EQ(mock_vcpu.arch.sbi_ctx.state, STARTED);
}

TEST(test_execute_reinits_health) {
    reset_mocks();
    struct vm_config cfg = make_recovery_config(0x80200000, true, 5);
    struct vm vm = make_test_vm(0, &cfg, 0);
    vm.health.status = VM_RECOVERING;
    vm.health.recovery_count = 3;
    vm.health.missed_count = 10;
    mock_vcpu.vm = &vm;

    if (setjmp(recovery_return) == 0) {
        vm_recovery_execute(&vm);
        ASSERT_TRUE(0);
    }

    /* health_monitor_init resets to HEALTHY */
    ASSERT_EQ(vm.health.status, VM_HEALTHY);
    ASSERT_EQ(vm.health.missed_count, 0);
    /* recovery_count is preserved (not reset by health_monitor_init) */
    ASSERT_EQ(vm.health.recovery_count, 3);
}

TEST(test_execute_three_barriers) {
    reset_mocks();
    struct vm_config cfg = make_recovery_config(0x80200000, true, 5);
    struct vm vm = make_test_vm(0, &cfg, 0);
    mock_vcpu.vm = &vm;

    if (setjmp(recovery_return) == 0) {
        vm_recovery_execute(&vm);
        ASSERT_TRUE(0);
    }

    ASSERT_EQ(mock.barrier_count, 3);
}

/* ================================================================== */
/*  TEST CASES — integration with health_monitor_check                 */
/* ================================================================== */

TEST(test_check_skips_recovering_vm) {
    reset_mocks();
    struct vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.id = 0;
    vm.health.status = VM_RECOVERING;
    vm.health.missed_count = 5;

    health_mock_time = HEALTH_MS_TO_TICKS(99999);
    health_monitor_check(&vm);

    /* Should not modify anything */
    ASSERT_EQ(vm.health.status, VM_RECOVERING);
    ASSERT_EQ(vm.health.missed_count, 5);
}

/* ================================================================== */
/*  Main test runner                                                   */
/* ================================================================== */

int main(void)
{
    printf("\n=== VM Recovery Unit Tests ===\n\n");

    /* vm_recovery_start guards */
    RUN_TEST(test_start_disabled_auto_recover);
    RUN_TEST(test_start_max_recoveries_exceeded);
    RUN_TEST(test_start_already_recovering);
    RUN_TEST(test_start_sets_recovering_state);
    RUN_TEST(test_start_increments_recovery_count);
    RUN_TEST(test_start_broadcasts_reset_ipi);
    RUN_TEST(test_start_calls_execute_on_master);
    RUN_TEST(test_start_skips_execute_on_non_master);
    RUN_TEST(test_start_unlimited_recoveries);

    /* vm_recovery_execute */
    RUN_TEST(test_execute_reinstalls_image);
    RUN_TEST(test_execute_resets_vcpu);
    RUN_TEST(test_execute_reinits_health);
    RUN_TEST(test_execute_three_barriers);

    /* Integration */
    RUN_TEST(test_check_skips_recovering_vm);

    printf("\n=== %d / %d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
