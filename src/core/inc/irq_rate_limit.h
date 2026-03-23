/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * IRQ Rate Limiting via Token Bucket Algorithm
 *
 * Prevents interrupt storms from degrading system availability in
 * Mixed-Criticality Systems. Each interrupt source gets a token bucket
 * whose parameters are set based on the owning VM's criticality level.
 *
 * When tokens are exhausted, IRQs are buffered (deferred) up to a
 * configurable limit. Only when the buffer overflows are IRQs dropped.
 * Deferred IRQs are drained automatically when tokens refill.
 */

#ifndef __IRQ_RATE_LIMIT_H__
#define __IRQ_RATE_LIMIT_H__

#ifdef IRQ_RL_TEST
#include <stdint.h>
#include <stdbool.h>
#else
#include <bao.h>
#endif

/**
 * Token bucket state for a single interrupt source.
 *
 * Tokens represent the "budget" for interrupt injections.
 * They refill at a steady rate and are consumed on each injection.
 */
struct irq_token_bucket {
    uint32_t tokens;          /**< Current available tokens */
    uint32_t max_tokens;      /**< Maximum burst capacity */
    uint32_t rate;            /**< Tokens replenished per second */
    uint32_t deferred_count;  /**< Buffered IRQs awaiting tokens */
    uint32_t max_deferred;    /**< Max buffer depth before dropping */
    uint64_t last_refill;     /**< Cycle count at last refill */
    uint64_t dropped;         /**< Total dropped count (statistics) */
};

/**
 * Rate limit result codes.
 */
enum irq_rl_action {
    IRQ_RL_ALLOW,    /**< Token consumed — inject the IRQ */
    IRQ_RL_DEFERRED, /**< No tokens — IRQ buffered for later injection */
    IRQ_RL_DROPPED,  /**< No tokens and buffer full — IRQ discarded */
};

/* ------------------------------------------------------------------ */
/*  Default parameters per criticality level                          */
/*  These can be overridden per-platform via compile-time defines.    */
/* ------------------------------------------------------------------ */

/** CRIT_HIGH: generous budget — safety-critical IRQs must not be starved */
#ifndef IRQ_RL_CRIT_HIGH_MAX_TOKENS
#define IRQ_RL_CRIT_HIGH_MAX_TOKENS    1000
#endif
#ifndef IRQ_RL_CRIT_HIGH_RATE
#define IRQ_RL_CRIT_HIGH_RATE          10000  /* tokens/sec */
#endif
#ifndef IRQ_RL_CRIT_HIGH_MAX_DEFERRED
#define IRQ_RL_CRIT_HIGH_MAX_DEFERRED  32
#endif

/** CRIT_LOW: tight budget — non-critical IRQs throttled aggressively */
#ifndef IRQ_RL_CRIT_LOW_MAX_TOKENS
#define IRQ_RL_CRIT_LOW_MAX_TOKENS     100
#endif
#ifndef IRQ_RL_CRIT_LOW_RATE
#define IRQ_RL_CRIT_LOW_RATE           1000   /* tokens/sec */
#endif
#ifndef IRQ_RL_CRIT_LOW_MAX_DEFERRED
#define IRQ_RL_CRIT_LOW_MAX_DEFERRED   8
#endif

/**
 * Platform timer frequency (cycles per second).
 * Must match the RISC-V timebase (QEMU virt: 10 MHz).
 * Override per-platform if different.
 */
#ifndef IRQ_RL_TIMER_FREQ
#define IRQ_RL_TIMER_FREQ              10000000ULL
#endif

/* ------------------------------------------------------------------ */
/*  Inline implementation                                             */
/* ------------------------------------------------------------------ */

/**
 * Read the monotonic cycle counter (RISC-V CSR cycle / 0xC00).
 * When IRQ_RL_TEST is defined, uses an external mock variable instead.
 */
#ifdef IRQ_RL_TEST
extern uint64_t irq_rl_mock_cycles;
static inline uint64_t irq_rl_read_cycles(void)
{
    return irq_rl_mock_cycles;
}
#else
static inline uint64_t irq_rl_read_cycles(void)
{
    uint64_t val;
    /* Use rdtime (CLINT timer @ 10 MHz) to match IRQ_RL_TIMER_FREQ.
     * rdcycle runs at CPU frequency which differs from the timer. */
    __asm__ volatile("rdtime %0" : "=r"(val));
    return val;
}
#endif

/**
 * Initialize a token bucket with criticality-appropriate parameters.
 *
 * @param b           Pointer to the bucket
 * @param criticality The VM's criticality level (CRIT_LOW=0, CRIT_HIGH=1)
 */
static inline void irq_bucket_init(struct irq_token_bucket *b,
                                   unsigned int criticality)
{
    if (criticality >= 1) {
        /* CRIT_HIGH */
        b->max_tokens   = IRQ_RL_CRIT_HIGH_MAX_TOKENS;
        b->rate         = IRQ_RL_CRIT_HIGH_RATE;
        b->max_deferred = IRQ_RL_CRIT_HIGH_MAX_DEFERRED;
    } else {
        /* CRIT_LOW */
        b->max_tokens   = IRQ_RL_CRIT_LOW_MAX_TOKENS;
        b->rate         = IRQ_RL_CRIT_LOW_RATE;
        b->max_deferred = IRQ_RL_CRIT_LOW_MAX_DEFERRED;
    }
    b->tokens        = b->max_tokens;  /* start with a full bucket */
    b->deferred_count = 0;
    b->last_refill   = irq_rl_read_cycles();
    b->dropped       = 0;
}

/**
 * Refill tokens based on elapsed time since last refill.
 *
 * Computes: new_tokens = elapsed_cycles * rate / timer_freq
 * Caps at max_tokens.
 *
 * @param b  Pointer to the bucket
 */
static inline void irq_bucket_refill(struct irq_token_bucket *b)
{
    uint64_t now = irq_rl_read_cycles();
    uint64_t elapsed = now - b->last_refill;

    /* Avoid division-by-zero and spurious refills */
    if (elapsed == 0) return;

    uint64_t new_tokens = (elapsed * (uint64_t)b->rate) / IRQ_RL_TIMER_FREQ;
    if (new_tokens > 0) {
        b->tokens += (uint32_t)new_tokens;
        if (b->tokens > b->max_tokens) {
            b->tokens = b->max_tokens;
        }
        b->last_refill = now;
    }
}

/**
 * Try to consume a token for an IRQ injection.
 *
 * Flow:
 *   1. Refill tokens from elapsed time
 *   2. Drain deferred IRQs first (they have priority — FIFO fairness)
 *   3. If tokens remain, consume one → ALLOW
 *   4. If no tokens and buffer not full → DEFERRED
 *   5. If no tokens and buffer full → DROPPED
 *
 * @param b  Pointer to the bucket
 * @return   IRQ_RL_ALLOW, IRQ_RL_DEFERRED, or IRQ_RL_DROPPED
 */
static inline enum irq_rl_action irq_bucket_consume(struct irq_token_bucket *b)
{
    /* Step 1: refill */
    irq_bucket_refill(b);

    /* Step 2: drain deferred IRQs that can now be serviced */
    while (b->deferred_count > 0 && b->tokens > 0) {
        b->deferred_count--;
        b->tokens--;
    }

    /* Step 3: try to consume for the current IRQ */
    if (b->tokens > 0) {
        b->tokens--;
        return IRQ_RL_ALLOW;
    }

    /* Step 4: buffer if space available */
    if (b->deferred_count < b->max_deferred) {
        b->deferred_count++;
        return IRQ_RL_DEFERRED;
    }

    /* Step 5: buffer full — must drop */
    b->dropped++;
    return IRQ_RL_DROPPED;
}

/**
 * Check if there are deferred IRQs that can now be drained.
 * Returns the number of IRQs drained (i.e., ready to inject).
 *
 * Call this periodically (e.g., on vplic_claim or timer tick) to
 * ensure deferred IRQs don't starve indefinitely.
 *
 * @param b  Pointer to the bucket
 * @return   Number of deferred IRQs drained
 */
static inline uint32_t irq_bucket_drain_deferred(struct irq_token_bucket *b)
{
    irq_bucket_refill(b);

    uint32_t drained = 0;
    while (b->deferred_count > 0 && b->tokens > 0) {
        b->deferred_count--;
        b->tokens--;
        drained++;
    }
    return drained;
}

#endif /* __IRQ_RATE_LIMIT_H__ */
