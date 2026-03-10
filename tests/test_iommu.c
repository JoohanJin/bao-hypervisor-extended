/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Host-side unit tests for basic IOMMU header values and encodings.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* Pull in the header under test (uses <bao.h>, resolved via Makefile -I) */
#include "../src/arch/riscv/inc/arch/iommu.h"

/* Minimal test harness (copied style from existing tests) */
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                              \
    do {                                                        \
        tests_run++;                                            \
        printf("  TEST %-50s ", #name);                       \
        fflush(stdout);                                         \
    } while (0)

#define PASS()                                                  \
    do { tests_passed++; printf("[PASS]\n"); } while (0)

#define FAIL(msg, ...)                                          \
    do {                                                        \
        tests_failed++;                                         \
        printf("[FAIL] " msg "\n", ##__VA_ARGS__);          \
    } while (0)

#define ASSERT_EQ(a, b)                                         \
    do {                                                        \
        if ((a) != (b)) {                                       \
            FAIL("Expected %llu == %llu at line %d",         \
                 (unsigned long long)(a),                      \
                 (unsigned long long)(b), __LINE__);           \
            return;                                             \
        }                                                       \
    } while (0)

#define ASSERT_TRUE(x)                                          \
    do {                                                        \
        if (!(x)) {                                             \
            FAIL("Assertion failed: %s at line %d",           \
                 #x, __LINE__);                                 \
            return;                                             \
        }                                                       \
    } while (0)

/* Test: Device Context size and DDT device count */
static void test_dc_size(void)
{
    TEST(dc_size);
    ASSERT_EQ(sizeof(struct riscv_iommu_dc), 64);
    ASSERT_EQ(RISCV_IOMMU_DDT_1LVL_MAX_DEVS, PAGE_SIZE / sizeof(struct riscv_iommu_dc));
    PASS();
}

/* Test: DDTP mode constants */
static void test_ddtp_modes(void)
{
    TEST(ddtp_modes);
    ASSERT_EQ(RISCV_IOMMU_DDTP_MODE_1LVL, 2);
    ASSERT_EQ(RISCV_IOMMU_DDTP_MODE_2LVL, 3);
    ASSERT_EQ(RISCV_IOMMU_DDTP_MODE_3LVL, 4);
    PASS();
}

/* Test: iohgatp field encoding/decoding */
static void test_iohgatp_encode(void)
{
    TEST(iohgatp_encode);

    uint64_t ppn = 0x12345ULL & RISCV_IOMMU_DC_IOHGATP_PPN_MSK;
    uint64_t gscid = 3ULL;
    uint64_t mode = RISCV_IOMMU_IOHGATP_SV39X4;

    uint64_t ioh = (mode << RISCV_IOMMU_DC_IOHGATP_MODE_OFF) |
                   ((gscid << RISCV_IOMMU_DC_IOHGATP_GSCID_OFF) & RISCV_IOMMU_DC_IOHGATP_GSCID_MSK) |
                   (ppn & RISCV_IOMMU_DC_IOHGATP_PPN_MSK);

    uint64_t extracted_mode = (ioh >> RISCV_IOMMU_DC_IOHGATP_MODE_OFF) & 0xFULL;
    uint64_t extracted_gscid = (ioh & RISCV_IOMMU_DC_IOHGATP_GSCID_MSK) >> RISCV_IOMMU_DC_IOHGATP_GSCID_OFF;
    uint64_t extracted_ppn = ioh & RISCV_IOMMU_DC_IOHGATP_PPN_MSK;

    ASSERT_EQ(extracted_mode, mode);
    ASSERT_EQ(extracted_gscid, gscid);
    ASSERT_EQ(extracted_ppn, ppn);
    PASS();
}

int main(void)
{
    printf("==== IOMMU Unit Tests ===\n\n");

    test_dc_size();
    test_ddtp_modes();
    test_iohgatp_encode();

    printf("\n==== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);

    return (tests_failed == 0) ? 0 : 1;
}
