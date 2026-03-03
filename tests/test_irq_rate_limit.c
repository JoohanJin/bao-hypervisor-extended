/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Unit tests for IRQ Rate Limiting (Token Bucket Algorithm)
 *
 * Build:  make -C tests/
 * Run:    ./tests/test_irq_rate_limit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ------------------------------------------------------------------ */
/*  Test-mode: mock the cycle counter                                 */
/* ------------------------------------------------------------------ */
#define IRQ_RL_TEST 1
uint64_t irq_rl_mock_cycles = 0;

/* Pull in the header under test */
#include "../src/core/inc/irq_rate_limit.h"

/* ------------------------------------------------------------------ */
/*  Minimal test harness                                              */
/* ------------------------------------------------------------------ */
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                              \
    do {                                                        \
        tests_run++;                                            \
        printf("  TEST %-50s ", #name);                         \
        fflush(stdout);                                         \
    } while (0)

#define PASS()                                                  \
    do { tests_passed++; printf("[PASS]\n"); } while (0)

#define FAIL(msg, ...)                                          \
    do {                                                        \
        tests_failed++;                                         \
        printf("[FAIL] " msg "\n", ##__VA_ARGS__);              \
    } while (0)

#define ASSERT_EQ(a, b)                                         \
    do {                                                        \
        if ((a) != (b)) {                                       \
            FAIL("Expected %llu == %llu at line %d",            \
                 (unsigned long long)(a),                        \
                 (unsigned long long)(b), __LINE__);            \
            return;                                             \
        }                                                       \
    } while (0)

#define ASSERT_TRUE(x)                                          \
    do {                                                        \
        if (!(x)) {                                             \
            FAIL("Assertion failed: %s at line %d",             \
                 #x, __LINE__);                                 \
            return;                                             \
        }                                                       \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Helper: reset mock clock                                          */
/* ------------------------------------------------------------------ */
static void reset_clock(uint64_t t)
{
    irq_rl_mock_cycles = t;
}

static void advance_clock(uint64_t delta)
{
    irq_rl_mock_cycles += delta;
}

/* ------------------------------------------------------------------ */
/*  Test cases                                                        */
/* ------------------------------------------------------------------ */

/**
 * 1. init_crit_high — CRIT_HIGH bucket gets correct parameters.
 */
static void test_init_crit_high(void)
{
    TEST(init_crit_high);
    struct irq_token_bucket b;
    reset_clock(1000);
    irq_bucket_init(&b, 1); /* CRIT_HIGH */

    ASSERT_EQ(b.max_tokens,   IRQ_RL_CRIT_HIGH_MAX_TOKENS);
    ASSERT_EQ(b.rate,         IRQ_RL_CRIT_HIGH_RATE);
    ASSERT_EQ(b.max_deferred, IRQ_RL_CRIT_HIGH_MAX_DEFERRED);
    ASSERT_EQ(b.tokens,       b.max_tokens);
    ASSERT_EQ(b.deferred_count, 0);
    ASSERT_EQ(b.dropped,      0);
    ASSERT_EQ(b.last_refill,  1000);
    PASS();
}

/**
 * 2. init_crit_low — CRIT_LOW bucket gets correct parameters.
 */
static void test_init_crit_low(void)
{
    TEST(init_crit_low);
    struct irq_token_bucket b;
    reset_clock(5000);
    irq_bucket_init(&b, 0); /* CRIT_LOW */

    ASSERT_EQ(b.max_tokens,   IRQ_RL_CRIT_LOW_MAX_TOKENS);
    ASSERT_EQ(b.rate,         IRQ_RL_CRIT_LOW_RATE);
    ASSERT_EQ(b.max_deferred, IRQ_RL_CRIT_LOW_MAX_DEFERRED);
    ASSERT_EQ(b.tokens,       b.max_tokens);
    ASSERT_EQ(b.deferred_count, 0);
    ASSERT_EQ(b.dropped,      0);
    PASS();
}

/**
 * 3. consume_with_tokens — basic consumption when tokens available.
 */
static void test_consume_with_tokens(void)
{
    TEST(consume_with_tokens);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 1); /* CRIT_HIGH, 1000 tokens */

    enum irq_rl_action act = irq_bucket_consume(&b);
    ASSERT_EQ(act, IRQ_RL_ALLOW);
    ASSERT_EQ(b.tokens, IRQ_RL_CRIT_HIGH_MAX_TOKENS - 1);
    PASS();
}

/**
 * 4. exhaust_tokens — consume all tokens, next should defer.
 */
static void test_exhaust_tokens(void)
{
    TEST(exhaust_tokens);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 0); /* CRIT_LOW, 100 tokens */

    /* Drain all tokens */
    for (uint32_t i = 0; i < IRQ_RL_CRIT_LOW_MAX_TOKENS; i++) {
        enum irq_rl_action act = irq_bucket_consume(&b);
        ASSERT_EQ(act, IRQ_RL_ALLOW);
    }

    ASSERT_EQ(b.tokens, 0);

    /* Next should be DEFERRED */
    enum irq_rl_action act = irq_bucket_consume(&b);
    ASSERT_EQ(act, IRQ_RL_DEFERRED);
    ASSERT_EQ(b.deferred_count, 1);
    PASS();
}

/**
 * 5. deferred_then_dropped — fill the deferred buffer, then get drops.
 */
static void test_deferred_then_dropped(void)
{
    TEST(deferred_then_dropped);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 0); /* CRIT_LOW, 100 tokens, 8 deferred */

    /* Drain all tokens */
    for (uint32_t i = 0; i < IRQ_RL_CRIT_LOW_MAX_TOKENS; i++) {
        irq_bucket_consume(&b);
    }

    /* Fill deferred buffer */
    for (uint32_t i = 0; i < IRQ_RL_CRIT_LOW_MAX_DEFERRED; i++) {
        enum irq_rl_action act = irq_bucket_consume(&b);
        ASSERT_EQ(act, IRQ_RL_DEFERRED);
    }
    ASSERT_EQ(b.deferred_count, IRQ_RL_CRIT_LOW_MAX_DEFERRED);

    /* Next should be DROPPED */
    enum irq_rl_action act = irq_bucket_consume(&b);
    ASSERT_EQ(act, IRQ_RL_DROPPED);
    ASSERT_EQ(b.dropped, 1);

    /* And another */
    act = irq_bucket_consume(&b);
    ASSERT_EQ(act, IRQ_RL_DROPPED);
    ASSERT_EQ(b.dropped, 2);
    PASS();
}

/**
 * 6. refill_after_time — tokens refill based on elapsed cycles.
 */
static void test_refill_after_time(void)
{
    TEST(refill_after_time);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 0); /* CRIT_LOW, rate=1000/s */

    /* Drain all tokens */
    for (uint32_t i = 0; i < IRQ_RL_CRIT_LOW_MAX_TOKENS; i++) {
        irq_bucket_consume(&b);
    }
    ASSERT_EQ(b.tokens, 0);

    /* Advance 0.1 seconds → should refill rate*0.1 = 100 tokens */
    advance_clock(IRQ_RL_TIMER_FREQ / 10);

    irq_bucket_refill(&b);
    ASSERT_EQ(b.tokens, 100);
    PASS();
}

/**
 * 7. refill_caps_at_max — refill doesn't exceed max_tokens.
 */
static void test_refill_caps_at_max(void)
{
    TEST(refill_caps_at_max);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 0); /* CRIT_LOW, max=100, rate=1000 */

    /* Consume 10 tokens */
    for (int i = 0; i < 10; i++) irq_bucket_consume(&b);
    ASSERT_EQ(b.tokens, 90);

    /* Advance 10 whole seconds — would add 10000 tokens, but cap to 100 */
    advance_clock(IRQ_RL_TIMER_FREQ * 10);
    irq_bucket_refill(&b);
    ASSERT_EQ(b.tokens, b.max_tokens);
    PASS();
}

/**
 * 8. consume_drains_deferred_first — deferred IRQs have priority over
 *    new IRQs when tokens become available.
 */
static void test_consume_drains_deferred_first(void)
{
    TEST(consume_drains_deferred_first);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 0); /* CRIT_LOW, 100 tokens, 8 deferred */

    /* Drain all tokens */
    for (uint32_t i = 0; i < IRQ_RL_CRIT_LOW_MAX_TOKENS; i++) {
        irq_bucket_consume(&b);
    }

    /* Queue 3 deferred */
    irq_bucket_consume(&b); /* deferred #1 */
    irq_bucket_consume(&b); /* deferred #2 */
    irq_bucket_consume(&b); /* deferred #3 */
    ASSERT_EQ(b.deferred_count, 3);

    /* Advance time to refill exactly 5 tokens (0.005s at 1000/s) */
    advance_clock(IRQ_RL_TIMER_FREQ * 5 / 1000);

    /* consume() should:
     *   1. refill → 5 tokens
     *   2. drain 3 deferred → 2 tokens left
     *   3. consume 1 for current IRQ → 1 token left → ALLOW
     */
    enum irq_rl_action act = irq_bucket_consume(&b);
    ASSERT_EQ(act, IRQ_RL_ALLOW);
    ASSERT_EQ(b.deferred_count, 0);
    ASSERT_EQ(b.tokens, 1);
    PASS();
}

/**
 * 9. drain_deferred_standalone — irq_bucket_drain_deferred
 *    returns the count of drained IRQs.
 */
static void test_drain_deferred_standalone(void)
{
    TEST(drain_deferred_standalone);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 0); /* CRIT_LOW */

    /* Drain all tokens */
    for (uint32_t i = 0; i < IRQ_RL_CRIT_LOW_MAX_TOKENS; i++) {
        irq_bucket_consume(&b);
    }

    /* Queue 5 deferred */
    for (int i = 0; i < 5; i++) irq_bucket_consume(&b);
    ASSERT_EQ(b.deferred_count, 5);

    /* Advance for 3 tokens worth of time */
    advance_clock(IRQ_RL_TIMER_FREQ * 3 / 1000);

    uint32_t drained = irq_bucket_drain_deferred(&b);
    ASSERT_EQ(drained, 3);
    ASSERT_EQ(b.deferred_count, 2);
    ASSERT_EQ(b.tokens, 0);
    PASS();
}

/**
 * 10. no_refill_at_zero_elapsed — no spurious token refill
 *     when clock hasn't advanced.
 */
static void test_no_refill_at_zero_elapsed(void)
{
    TEST(no_refill_at_zero_elapsed);
    struct irq_token_bucket b;
    reset_clock(1000);
    irq_bucket_init(&b, 1);

    /* Consume some tokens */
    for (int i = 0; i < 50; i++) irq_bucket_consume(&b);
    uint32_t before = b.tokens;

    /* Don't advance clock — refill should be a no-op */
    irq_bucket_refill(&b);
    ASSERT_EQ(b.tokens, before);
    PASS();
}

/**
 * 11. crit_high_more_generous — CRIT_HIGH has strictly more budget
 *     than CRIT_LOW.
 */
static void test_crit_high_more_generous(void)
{
    TEST(crit_high_more_generous);
    struct irq_token_bucket hi, lo;
    reset_clock(0);
    irq_bucket_init(&hi, 1);
    irq_bucket_init(&lo, 0);

    ASSERT_TRUE(hi.max_tokens   > lo.max_tokens);
    ASSERT_TRUE(hi.rate         > lo.rate);
    ASSERT_TRUE(hi.max_deferred > lo.max_deferred);
    PASS();
}

/**
 * 12. dropped_counter_accumulates — dropped field counts total drops.
 */
static void test_dropped_counter_accumulates(void)
{
    TEST(dropped_counter_accumulates);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 0); /* CRIT_LOW, max_deferred=8 */

    /* Drain all tokens and fill deferred buffer */
    for (uint32_t i = 0; i < IRQ_RL_CRIT_LOW_MAX_TOKENS + IRQ_RL_CRIT_LOW_MAX_DEFERRED; i++) {
        irq_bucket_consume(&b);
    }

    /* Now drop 10 */
    for (int i = 0; i < 10; i++) {
        enum irq_rl_action act = irq_bucket_consume(&b);
        ASSERT_EQ(act, IRQ_RL_DROPPED);
    }
    ASSERT_EQ(b.dropped, 10);
    PASS();
}

/**
 * 13. partial_deferred_drain — when only some deferred can be drained,
 *     the remainder stays buffered.
 */
static void test_partial_deferred_drain(void)
{
    TEST(partial_deferred_drain);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 0);

    /* Drain all */
    for (uint32_t i = 0; i < IRQ_RL_CRIT_LOW_MAX_TOKENS; i++) {
        irq_bucket_consume(&b);
    }

    /* Queue 5 deferred */
    for (int i = 0; i < 5; i++) irq_bucket_consume(&b);
    ASSERT_EQ(b.deferred_count, 5);

    /* Advance for exactly 2 tokens */
    advance_clock(IRQ_RL_TIMER_FREQ * 2 / 1000);

    /* Consume: refills 2, drains 2 from deferred, 0 for new → DEFERRED */
    enum irq_rl_action act = irq_bucket_consume(&b);
    ASSERT_EQ(act, IRQ_RL_DEFERRED);
    ASSERT_EQ(b.deferred_count, 4); /* 5 - 2 drained + 1 new deferred = 4 */
    PASS();
}

/**
 * 14. full_burst_then_recover — simulate burst, time passes, full recovery.
 */
static void test_full_burst_then_recover(void)
{
    TEST(full_burst_then_recover);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 0); /* CRIT_LOW: max=100, rate=1000/s */

    /* Burst: drain all tokens + fill deferred */
    for (uint32_t i = 0; i < IRQ_RL_CRIT_LOW_MAX_TOKENS; i++) {
        ASSERT_EQ(irq_bucket_consume(&b), IRQ_RL_ALLOW);
    }
    for (uint32_t i = 0; i < IRQ_RL_CRIT_LOW_MAX_DEFERRED; i++) {
        ASSERT_EQ(irq_bucket_consume(&b), IRQ_RL_DEFERRED);
    }
    /* One more is dropped */
    ASSERT_EQ(irq_bucket_consume(&b), IRQ_RL_DROPPED);

    /* Wait 1 full second → refills 1000 tokens, capped to 100 */
    advance_clock(IRQ_RL_TIMER_FREQ);

    /* First consume drains deferred (8 of them), then allows current */
    enum irq_rl_action act = irq_bucket_consume(&b);
    ASSERT_EQ(act, IRQ_RL_ALLOW);
    ASSERT_EQ(b.deferred_count, 0);
    /* tokens should be max(100) - 8(deferred drained) - 1(current) = 91 */
    ASSERT_EQ(b.tokens, 91);
    PASS();
}

/**
 * 15. tiny_time_advance — very small time advance doesn't generate tokens.
 */
static void test_tiny_time_advance(void)
{
    TEST(tiny_time_advance);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 0); /* rate=1000/s at 10MHz → 1 token per 10000 cycles */

    /* Consume 1 */
    irq_bucket_consume(&b);
    uint32_t after = b.tokens;

    /* Advance only 100 cycles — less than 1 token */
    advance_clock(100);
    irq_bucket_refill(&b);
    ASSERT_EQ(b.tokens, after); /* no change */
    PASS();
}

/**
 * 16. exactly_one_token_refill — precise single-token refill.
 */
static void test_exactly_one_token_refill(void)
{
    TEST(exactly_one_token_refill);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 0); /* CRIT_LOW: rate=1000, freq=10MHz → 10000 cycles/token */

    /* Drain all */
    for (uint32_t i = 0; i < IRQ_RL_CRIT_LOW_MAX_TOKENS; i++) {
        irq_bucket_consume(&b);
    }
    ASSERT_EQ(b.tokens, 0);

    /* Advance exactly 10000 cycles (= 1 token at 1000 tokens/s) */
    advance_clock(10000);
    irq_bucket_refill(&b);
    ASSERT_EQ(b.tokens, 1);
    PASS();
}

/**
 * 17. multi_deferred_partial_time — advance gives fewer tokens
 *     than deferred count, check partial drain.
 */
static void test_multi_deferred_partial_time(void)
{
    TEST(multi_deferred_partial_time);
    struct irq_token_bucket b;
    reset_clock(0);
    irq_bucket_init(&b, 1); /* CRIT_HIGH: max=1000, rate=10000 */

    /* Drain all tokens */
    for (uint32_t i = 0; i < IRQ_RL_CRIT_HIGH_MAX_TOKENS; i++) {
        irq_bucket_consume(&b);
    }

    /* Queue 20 deferred */
    for (int i = 0; i < 20; i++) irq_bucket_consume(&b);
    ASSERT_EQ(b.deferred_count, 20);

    /* Advance for exactly 5 tokens: 10000/s at 10MHz → 1000 cycles/token
       5 tokens = 5000 cycles */
    advance_clock(5000);
    uint32_t drained = irq_bucket_drain_deferred(&b);
    ASSERT_EQ(drained, 5);
    ASSERT_EQ(b.deferred_count, 15);
    PASS();
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                       */
/* ------------------------------------------------------------------ */
int main(void)
{
    printf("\n==== IRQ Rate Limit Unit Tests ====\n\n");

    test_init_crit_high();
    test_init_crit_low();
    test_consume_with_tokens();
    test_exhaust_tokens();
    test_deferred_then_dropped();
    test_refill_after_time();
    test_refill_caps_at_max();
    test_consume_drains_deferred_first();
    test_drain_deferred_standalone();
    test_no_refill_at_zero_elapsed();
    test_crit_high_more_generous();
    test_dropped_counter_accumulates();
    test_partial_deferred_drain();
    test_full_burst_then_recover();
    test_tiny_time_advance();
    test_exactly_one_token_refill();
    test_multi_deferred_partial_time();

    printf("\n==== Results: %d/%d passed, %d failed ====\n\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
