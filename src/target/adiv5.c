/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2015  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2018 - 2021 Uwe Bonnes
 *                           (bon@elektron.ikp.physik.tu-darmstadt.de)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements the transport generic functions.
 * See the following ARM Reference Documents:
 *
 * ARM Debug Interface v5 Architecure Specification, ARM IHI 0031E
 */
#include "general.h"
#include "target.h"
#include "target_internal.h"
#include "target_probe.h"
#include "adiv5.h"
#include "cortexm.h"
#include "exception.h"

/* All this should probably be defined in a dedicated ADIV5 header, so that they
 * are consistently named and accessible when needed in the codebase.
 */

/* Values from ST RM0436 (STM32MP157), 66.9 APx_IDR
 * and ST RM0438 (STM32L5) 52.3.1, AP_IDR */
#define ARM_AP_TYPE_AHB  1
#define ARM_AP_TYPE_APB  3
#define ARM_AP_TYPE_AXI  4
#define ARM_AP_TYPE_AHB5 5

/* ROM table CIDR values */
#define CIDR0_OFFSET 0xFF0 /* DBGCID0 */
#define CIDR1_OFFSET 0xFF4 /* DBGCID1 */
#define CIDR2_OFFSET 0xFF8 /* DBGCID2 */
#define CIDR3_OFFSET 0xFFC /* DBGCID3 */

/* Component class ID register can be broken down into the following logical
 * interpretation of the 32bit value consisting of the least significant bytes
 * of the 4 CID registers:
 * |7   ID3 reg   0|7   ID2 reg   0|7   ID1 reg   0|7   ID0 reg   0|
 * |1|0|1|1|0|0|0|1|0|0|0|0|0|1|0|1| | | | |0|0|0|0|0|0|0|0|1|1|0|1|
 * |31           24|23           16|15   12|11     |              0|
 * \_______________ ______________/\___ __/\___________ ___________/
 *                 V                   V               V
 *             Preamble            Component       Preamble
 *                                   Class
 * \_______________________________ _______________________________/
 *                                 V
 *                           Component ID
 */
#define CID_PREAMBLE    UINT32_C(0xB105000D)
#define CID_CLASS_MASK  UINT32_C(0x0000F000)
#define CID_CLASS_SHIFT 12U

/* The following enum is based on the Component Class value table 13-3 of the
 * ADIv5 standard.
 */
enum cid_class {
	cidc_gvc = 0x0,    /* Generic verification component*/
	cidc_romtab = 0x1, /* ROM Table, std. layout (ADIv5 Chapter 14) */
	/* 0x2 - 0x8 */    /* Reserved */
	cidc_dc = 0x9,     /* Debug component, std. layout (CoreSight Arch. Spec.) */
	/* 0xA */          /* Reserved */
	cidc_ptb = 0xB,    /* Peripheral Test Block (PTB) */
	/* 0xC */          /* Reserved */
	cidc_dess = 0xD,   /* OptimoDE Data Engine SubSystem (DESS) component */
	cidc_gipc = 0xE,   /* Generic IP Component */
	cidc_sys = 0xF,    /* CoreLink, PrimeCell, or other system component with no standard register layout */
	cidc_unknown = 0x10
};

#ifdef ENABLE_DEBUG
/* The reserved ones only have an R in them, to save a bit of space. */
static const char *const cidc_debug_strings[] = {
	[cidc_gvc] = "Generic verification component",            /* 0x0 */
	[cidc_romtab] = "ROM Table",                              /* 0x1 */
	[0x2 ... 0x8] = "R",                                      /* 0x2 - 0x8 */
	[cidc_dc] = "Debug component",                            /* 0x9 */
	[0xA] = "R",                                              /* 0xA */
	[cidc_ptb] = "Peripheral Test Block",                     /* 0xB */
	[0xC] = "R",                                              /* 0xC */
	[cidc_dess] = "OptimoDE Data Engine SubSystem component", /* 0xD */
	[cidc_gipc] = "Generic IP component",                     /* 0xE */
	[cidc_sys] = "Non STD System component",                  /* 0xF */
	[cidc_unknown] = "Unknown component class"                /* 0x10 */
};
#endif

#define PIDR0_OFFSET 0xFE0 /* DBGPID0 */
#define PIDR1_OFFSET 0xFE4 /* DBGPID1 */
#define PIDR2_OFFSET 0xFE8 /* DBGPID2 */
#define PIDR3_OFFSET 0xFEC /* DBGPID3 */
#define PIDR4_OFFSET 0xFD0 /* DBGPID4 */
#define PIDR5_OFFSET 0xFD4 /* DBGPID5 (Reserved) */
#define PIDR6_OFFSET 0xFD8 /* DBGPID6 (Reserved) */
#define PIDR7_OFFSET 0xFDC /* DBGPID7 (Reserved) */

#define PIDR_JEP106_CONT_OFFSET 32ULL                                /*JEP-106 Continuation Code offset */
#define PIDR_JEP106_CONT_MASK   (0xFULL << PIDR_JEP106_CONT_OFFSET)  /*JEP-106 Continuation Code mask */
#define PIDR_REV_OFFSET         20ULL                                /* Revision bits offset */
#define PIDR_REV_MASK           (0xFFFULL << PIDR_REV_OFFSET)        /* Revision bits mask */
#define PIDR_JEP106_USED_OFFSET 19ULL                                /* JEP-106 code used flag offset */
#define PIDR_JEP106_USED        (1ULL << PIDR_JEP106_USED_OFFSET)    /* JEP-106 code used flag */
#define PIDR_JEP106_CODE_OFFSET 12ULL                                /* JEP-106 code offset */
#define PIDR_JEP106_CODE_MASK   (0x7FULL << PIDR_JEP106_CODE_OFFSET) /* JEP-106 code mask */
#define PIDR_PN_MASK            (0xFFFULL)                           /* Part number */

#define DEVTYPE_OFFSET 0xFCCU /* CoreSight Device Type Register */
#define DEVARCH_OFFSET 0xFBCU /* CoreSight Device Architecture Register */

#define DEVTYPE_MASK        0x000000FFU
#define DEVARCH_PRESENT     (1U << 20)
#define DEVARCH_ARCHID_MASK 0x0000FFFFU

enum arm_arch {
	aa_nosupport,
	aa_cortexm,
	aa_cortexa,
	aa_end
};

#ifdef ENABLE_DEBUG
#define ARM_COMPONENT_STR(...) __VA_ARGS__
#else
#define ARM_COMPONENT_STR(...)
#endif

/* The part number list was adopted from OpenOCD:
 * https://sourceforge.net/p/openocd/code/ci/406f4/tree/src/target/arm_adi_v5.c#l932
 *
 * The product ID register consists of several parts. For a full description
 * refer to ARM Debug Interface v5 Architecture Specification. Based on the
 * document the pidr is 64 bit long and has the following interpratiation:
 * |7   ID7 reg   0|7   ID6 reg   0|7   ID5 reg   0|7   ID4 reg   0|
 * |0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0| | | | | | | | |
 * |63           56|55           48|47           40|39   36|35   32|
 * \_______________________ ______________________/\___ __/\___ ___/
 *                         V                           V       V
 *                    Reserved, RAZ                   4KB      |
 *                                                   count     |
 *                                                          JEP-106
 *                                                     Continuation Code (only valid for JEP-106 codes)
 *
 * |7   ID3 reg   0|7   ID2 reg   0|7   ID1 reg   0|7   ID0 reg   0|
 * | | | | | | | | | | | | | | | | | | | | | | | | | | | | | | | | |
 * |31   28|27   24|23   20|||18   |     12|11     |              0|
 * \___ __/\__ ___/\___ __/ |\______ _____/\___________ ___________/
 *     V      V        V    |       V                  V
 *  RevAnd    |    Revision |  JEP-106 ID         Part number
 *            |             |  (no parity)
 *        Customer          19
 *        modified          `- JEP-106 code is used
 *
 * only a subset of Part numbers are listed,
 * the ones that have ARM as the designer code.
 *
 * To properly identify ADIv6 CoreSight components, two additional fields,
 * DEVTYPE and ARCHID are read.
 * The dev_type and arch_id values in the table below were found in the
 * corresponding logic in pyOCD:
 * https://github.com/mbedmicro/pyOCD/blob/master/pyocd/coresight/component_ids.py
 *
 * Additional reference on the DEVTYPE and DEVARCH registers can be found in the
 * ARM CoreSight Architecture Specification v3.0, sections B2.3.4 and B2.3.8.
 */
static const struct {
	uint16_t part_number;
	uint8_t dev_type;
	uint16_t arch_id;
	enum arm_arch arch;
	enum cid_class cidc;
#ifdef ENABLE_DEBUG
	const char *type;
	const char *full;
#endif
} arm_component_lut[] = {
	{0x000, 0x00, 0, aa_cortexm, cidc_gipc, ARM_COMPONENT_STR("Cortex-M3 SCS", "(System Control Space)")},
	{0x001, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M3 ITM", "(Instrumentation Trace Module)")},
	{0x002, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M3 DWT", "(Data Watchpoint and Trace)")},
	{0x003, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M3 FBP", "(Flash Patch and Breakpoint)")},
	{0x008, 0x00, 0, aa_cortexm, cidc_gipc, ARM_COMPONENT_STR("Cortex-M0 SCS", "(System Control Space)")},
	{0x00a, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M0 DWT", "(Data Watchpoint and Trace)")},
	{0x00b, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M0 BPU", "(Breakpoint Unit)")},
	{0x00c, 0x00, 0, aa_cortexm, cidc_gipc, ARM_COMPONENT_STR("Cortex-M4 SCS", "(System Control Space)")},
	{0x00d, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight ETM11", "(Embedded Trace)")},
	{0x00e, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M7 FBP", "(Flash Patch and Breakpoint)")},
	{0x101, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("System TSGEN", "(Time Stamp Generator)")},
	{0x471, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M0  ROM", "(Cortex-M0 ROM)")},
	{0x490, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-A15 GIC", "(Generic Interrupt Controller)")},
	{0x4c0, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M0+ ROM", "(Cortex-M0+ ROM)")},
	{0x4c3, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M3 ROM", "(Cortex-M3 ROM)")},
	{0x4c4, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M4 ROM", "(Cortex-M4 ROM)")},
	{0x4c7, 0x00, 0, aa_nosupport, cidc_unknown,
		ARM_COMPONENT_STR("Cortex-M7 PPB", "(Cortex-M7 Private Peripheral Bus ROM Table)")},
	{0x4c8, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M7 ROM", "(Cortex-M7 ROM)")},
	{0x906, 0x14, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight CTI", "(Cross Trigger)")},
	{0x907, 0x21, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight ETB", "(Trace Buffer)")},
	{0x908, 0x12, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight CSTF", "(Trace Funnel)")},
	{0x910, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight ETM9", "(Embedded Trace)")},
	{0x912, 0x11, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight TPIU", "(Trace Port Interface Unit)")},
	{0x913, 0x00, 0, aa_nosupport, cidc_unknown,
		ARM_COMPONENT_STR("CoreSight ITM", "(Instrumentation Trace Macrocell)")},
	{0x914, 0x11, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight SWO", "(Single Wire Output)")},
	{0x917, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight HTM", "(AHB Trace Macrocell)")},
	{0x920, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight ETM11", "(Embedded Trace)")},
	{0x921, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-A8 ETM", "(Embedded Trace)")},
	{0x922, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-A8 CTI", "(Cross Trigger)")},
	{0x923, 0x11, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M3 TPIU", "(Trace Port Interface Unit)")},
	{0x924, 0x13, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M3 ETM", "(Embedded Trace)")},
	{0x925, 0x13, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M4 ETM", "(Embedded Trace)")},
	{0x930, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-R4 ETM", "(Embedded Trace)")},
	{0x932, 0x31, 0x0a31, aa_nosupport, cidc_unknown,
		ARM_COMPONENT_STR("CoreSight MTB-M0+", "(Simple Execution Trace)")},
	{0x941, 0x00, 0, aa_nosupport, cidc_unknown,
		ARM_COMPONENT_STR("CoreSight TPIU-Lite", "(Trace Port Interface Unit)")},
	{0x950, 0x00, 0, aa_nosupport, cidc_unknown,
		ARM_COMPONENT_STR("CoreSight Component", "(unidentified Cortex-A9 component)")},
	{0x955, 0x00, 0, aa_nosupport, cidc_unknown,
		ARM_COMPONENT_STR("CoreSight Component", "(unidentified Cortex-A5 component)")},
	{0x956, 0x13, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-A7 ETM", "(Embedded Trace)")},
	{0x95f, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-A15 PTM", "(Program Trace Macrocell)")},
	{0x961, 0x32, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight TMC", "(Trace Memory Controller)")},
	{0x962, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight STM", "(System Trace Macrocell)")},
	{0x963, 0x63, 0x0a63, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight STM", "(System Trace Macrocell)")},
	{0x975, 0x13, 0x4a13, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M7 ETM", "(Embedded Trace)")},
	{0x9a0, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("CoreSight PMU", "(Performance Monitoring Unit)")},
	{0x9a1, 0x11, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M4 TPIU", "(Trace Port Interface Unit)")},
	{0x9a6, 0x14, 0x1a14, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M0+ CTI", "(Cross Trigger Interface)")},
	{0x9a9, 0x11, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-M7 TPIU", "(Trace Port Interface Unit)")},
	{0x9a5, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-A5 ETM", "(Embedded Trace)")},
	{0x9a7, 0x16, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-A7 PMU", "(Performance Monitor Unit)")},
	{0x9af, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-A15 PMU", "(Performance Monitor Unit)")},
	{0xc05, 0x00, 0, aa_cortexa, cidc_dc, ARM_COMPONENT_STR("Cortex-A5 Debug", "(Debug Unit)")},
	{0xc07, 0x15, 0, aa_cortexa, cidc_dc, ARM_COMPONENT_STR("Cortex-A7 Debug", "(Debug Unit)")},
	{0xc08, 0x00, 0, aa_cortexa, cidc_dc, ARM_COMPONENT_STR("Cortex-A8 Debug", "(Debug Unit)")},
	{0xc09, 0x00, 0, aa_cortexa, cidc_dc, ARM_COMPONENT_STR("Cortex-A9 Debug", "(Debug Unit)")},
	{0xc0f, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-A15 Debug", "(Debug Unit)")}, /* support? */
	{0xc14, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Cortex-R4 Debug", "(Debug Unit)")},  /* support? */
	{0xcd0, 0x00, 0, aa_nosupport, cidc_unknown, ARM_COMPONENT_STR("Atmel DSU", "(Device Service Unit)")},
	{0xd20, 0x00, 0x2a04, aa_cortexm, cidc_dc, ARM_COMPONENT_STR("Cortex-M23", "(System Control Space)")},
	{0xd20, 0x11, 0, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M23", "(Trace Port Interface Unit)")},
	{0xd20, 0x13, 0, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M23", "(Embedded Trace)")},
	{0xd20, 0x31, 0x0a31, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M23", "(Micro Trace Buffer)")},
	{0xd20, 0x00, 0x1a02, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M23", "(Data Watchpoint and Trace)")},
	{0xd20, 0x00, 0x1a03, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M23", "(Breakpoint Unit)")},
	{0xd20, 0x14, 0x1a14, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M23", "(Cross Trigger)")},
	{0xd21, 0x00, 0x2a04, aa_cortexm, cidc_dc, ARM_COMPONENT_STR("Cortex-M33", "(System Control Space)")},
	{0xd21, 0x31, 0x0a31, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M33", "(Micro Trace Buffer)")},
	{0xd21, 0x43, 0x1a01, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M33", "(Instrumentation Trace Macrocell)")},
	{0xd21, 0x00, 0x1a02, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M33", "(Data Watchpoint and Trace)")},
	{0xd21, 0x00, 0x1a03, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M33", "(Breakpoint Unit)")},
	{0xd21, 0x14, 0x1a14, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M33", "(Cross Trigger)")},
	{0xd21, 0x13, 0x4a13, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M33", "(Embedded Trace)")},
	{0xd21, 0x11, 0, aa_nosupport, cidc_dc, ARM_COMPONENT_STR("Cortex-M33", "(Trace Port Interface Unit)")},
	{0xfff, 0x00, 0, aa_end, cidc_unknown, ARM_COMPONENT_STR("end", "end")},
};

/* Used to probe for a protected SAMX5X device */
#define SAMX5X_DSU_CTRLSTAT 0x41002100U
#define SAMX5X_STATUSB_PROT (1U << 16U)

void adiv5_ap_ref(ADIv5_AP_t *ap)
{
	if (ap->refcnt == 0)
		ap->dp->refcnt++;
	ap->refcnt++;
}

static void adiv5_dp_unref(ADIv5_DP_t *dp)
{
	if (--(dp->refcnt) == 0)
		free(dp);
}

void adiv5_ap_unref(ADIv5_AP_t *ap)
{
	if (--(ap->refcnt) == 0) {
		adiv5_dp_unref(ap->dp);
		free(ap);
	}
}

static uint32_t adiv5_mem_read32(ADIv5_AP_t *ap, uint32_t addr)
{
	uint32_t ret;
	adiv5_mem_read(ap, &ret, addr, sizeof(ret));
	return ret;
}

static uint32_t adiv5_ap_read_id(ADIv5_AP_t *ap, uint32_t addr)
{
	uint32_t res = 0;
	uint8_t data[16];
	adiv5_mem_read(ap, data, addr, sizeof(data));
	for (size_t i = 0; i < 4; ++i)
		res |= (data[4U * i] << (i * 8U));
	return res;
}

uint64_t adiv5_ap_read_pidr(ADIv5_AP_t *ap, uint32_t addr)
{
	uint64_t pidr = adiv5_ap_read_id(ap, addr + PIDR4_OFFSET);
	pidr = pidr << 32 | adiv5_ap_read_id(ap, addr + PIDR0_OFFSET);
	return pidr;
}

/* Halt CortexM
 *
 * Run in tight loop to catch small windows of awakeness.
 * Repeat the write command with the highest possible value
 * of the trannsaction counter, if not on MINDP
 */
static uint32_t cortexm_initial_halt(ADIv5_AP_t *ap)
{
	const uint32_t ctrlstat = adiv5_dp_read(ap->dp, ADIV5_DP_CTRLSTAT);

	const uint32_t dhcsr_ctl = CORTEXM_DHCSR_DBGKEY | CORTEXM_DHCSR_C_DEBUGEN | CORTEXM_DHCSR_C_HALT;
	const uint32_t dhcsr_valid = CORTEXM_DHCSR_S_HALT | CORTEXM_DHCSR_C_DEBUGEN;
	const bool use_low_access = !ap->dp->mindp;

	platform_timeout halt_timeout;
	platform_timeout_set(&halt_timeout, cortexm_wait_timeout);

	if (use_low_access) {
		/* ap_mem_access_setup() sets ADIV5_AP_CSW_ADDRINC_SINGLE -> unusable!*/
		adiv5_ap_write(ap, ADIV5_AP_CSW, ap->csw | ADIV5_AP_CSW_SIZE_WORD);
		adiv5_dp_low_access(ap->dp, ADIV5_LOW_WRITE, ADIV5_AP_TAR, CORTEXM_DHCSR);
	}

	/* Workaround for CMSIS-DAP Bulk orbtrace
	 * High values of TRNCNT lead to NO_ACK answer from debugger.
	 *
	 * However CMSIS/HID even with highest value has few chances to catch
	 * a STM32F767 mostly sleeping in WFI!
	 */
	uint32_t start_time = platform_time_ms();
	uint32_t trncnt = 0x80U;
	bool reset_seen = false;
	while (!platform_timeout_is_expired(&halt_timeout)) {
		uint32_t dhcsr;

		if (use_low_access) {
			adiv5_dp_low_access(
				ap->dp, ADIV5_LOW_WRITE, ADIV5_DP_CTRLSTAT, ctrlstat | (trncnt * ADIV5_DP_CTRLSTAT_TRNCNT));
			adiv5_dp_low_access(ap->dp, ADIV5_LOW_WRITE, ADIV5_AP_DRW, dhcsr_ctl);
			if (trncnt < 0xfffU) {
				trncnt += (platform_time_ms() - start_time) * 8U;
			} else {
				trncnt = 0xfffU;
			}
			dhcsr = adiv5_dp_low_access(ap->dp, ADIV5_LOW_READ, ADIV5_AP_DRW, 0);
		} else {
			adiv5_mem_write(ap, CORTEXM_DHCSR, &dhcsr_ctl, sizeof(dhcsr_ctl));
			dhcsr = adiv5_mem_read32(ap, CORTEXM_DHCSR);
		}

		/* ADIV5_DP_CTRLSTAT_READOK is always set e.g. on STM32F7 even so
		   CORTEXM_DHCS reads nonsense*/
		/* On a sleeping STM32F7, invalid DHCSR reads with e.g. 0xffffffff and
		 * 0x0xA05F0000  may happen.
		 * M23/33 will have S_SDE set when debug is allowed
		 */
		if ((dhcsr != 0xffffffffU) &&       /* Invalid read */
			((dhcsr & 0xf000fff0U) == 0)) { /* Check RAZ bits */
			if ((dhcsr & CORTEXM_DHCSR_S_RESET_ST) && !reset_seen) {
				if (connect_assert_nrst)
					return dhcsr;
				reset_seen = true;
				continue;
			}
			if ((dhcsr & dhcsr_valid) == dhcsr_valid) /* Halted */
				return dhcsr;
		}
	}

	return 0;
}

/* Prepare to read SYSROM and SYSROM PIDR
 *
 * Try hard to halt, if not connecting under reset
 * Request TRCENA and default vector catch
 * release from reset when connecting under reset.
 *
 * E.g. Stm32F7
 * - fails reading romtable in WFI
 * - fails with some AP accesses when romtable is read under reset.
 * - fails reading some ROMTABLE entries w/o TRCENA
 * - fails reading outside SYSROM when halted from WFI and
 *   DBGMCU_CR not set.
 *
 * E.g. Stm32F0
 * - fails reading DBGMCU when under reset
 *
 * Keep a copy of DEMCR at startup to restore with exit, to
 * not interrupt tracing initiated by the CPU.
 */
static bool cortexm_prepare(ADIv5_AP_t *ap)
{
#if ((PC_HOSTED == 1) || (ENABLE_DEBUG == 1))
	uint32_t start_time = platform_time_ms();
#endif
	uint32_t dhcsr = cortexm_initial_halt(ap);
	if (!dhcsr) {
		DEBUG_WARN("Halt via DHCSR: Failure DHCSR %08" PRIx32 " after % " PRId32 "ms\nTry again, evt. with longer "
				   "timeout or connect under reset\n",
			adiv5_mem_read32(ap, CORTEXM_DHCSR), platform_time_ms() - start_time);
		return false;
	}
	DEBUG_INFO("Halt via DHCSR: success %08" PRIx32 " after %" PRId32 "ms\n", dhcsr, platform_time_ms() - start_time);
	ap->ap_cortexm_demcr = adiv5_mem_read32(ap, CORTEXM_DEMCR);
	const uint32_t demcr = CORTEXM_DEMCR_TRCENA | CORTEXM_DEMCR_VC_HARDERR | CORTEXM_DEMCR_VC_CORERESET;
	adiv5_mem_write(ap, CORTEXM_DEMCR, &demcr, sizeof(demcr));
	platform_timeout reset_timeout;
	platform_timeout_set(&reset_timeout, cortexm_wait_timeout);
	platform_nrst_set_val(false);
	while (1) {
		dhcsr = adiv5_mem_read32(ap, CORTEXM_DHCSR);
		if (!(dhcsr & CORTEXM_DHCSR_S_RESET_ST))
			break;
		if (platform_timeout_is_expired(&reset_timeout)) {
			DEBUG_WARN("Error releasing from reset\n");
			return false;
		}
	}
	return true;
}

/* Return true if we find a debuggable device.*/
static void adiv5_component_probe(ADIv5_AP_t *ap, uint32_t addr, const size_t recursion, const uint32_t num_entry)
{
	(void)num_entry;

	addr &= 0xfffff000U; /* Mask out base address */
	if (addr == 0)       /* No rom table on this AP */
		return;

	const volatile uint32_t cidr = adiv5_ap_read_id(ap, addr + CIDR0_OFFSET);
	if (ap->dp->fault) {
		DEBUG_WARN("CIDR read timeout on AP%d, aborting.\n", ap->apsel);
		return;
	}
	if ((cidr & ~CID_CLASS_MASK) != CID_PREAMBLE)
		return;

#if defined(ENABLE_DEBUG)
	char indent[recursion + 1];

	for (size_t i = 0; i < recursion; i++)
		indent[i] = ' ';
	indent[recursion] = 0;
#endif

	if (adiv5_dp_error(ap->dp)) {
		DEBUG_WARN("%sFault reading ID registers\n", indent);
		return;
	}

	/* CIDR preamble sanity check */
	if ((cidr & ~CID_CLASS_MASK) != CID_PREAMBLE) {
		DEBUG_WARN("%s%" PRIu32 " 0x%08" PRIx32 ": 0x%08" PRIx32 " <- does not match preamble (0x%08" PRIx32 ")\n",
			indent + 1, num_entry, addr, cidr, CID_PREAMBLE);
		return;
	}

	/* Extract Component ID class nibble */
	const uint32_t cid_class = (cidr & CID_CLASS_MASK) >> CID_CLASS_SHIFT;
	const uint64_t pidr = adiv5_ap_read_pidr(ap, addr);

	uint16_t designer_code;
	if (pidr & PIDR_JEP106_USED) {
		/* (OFFSET - 8) because we want it on bits 11:8 of new code, see "JEP-106 code list" */
		designer_code = (pidr & PIDR_JEP106_CONT_MASK) >> (PIDR_JEP106_CONT_OFFSET - 8) |
		                (pidr & PIDR_JEP106_CODE_MASK) >> PIDR_JEP106_CODE_OFFSET;

		if (designer_code == JEP106_MANUFACTURER_ERRATA_STM32WX || designer_code == JEP106_MANUFACTURER_ERRATA_CS) {
			/**
			 * see 'JEP-106 code list' for context, here we are aliasing codes that are non compliant with the
			 * JEP-106 standard to their expected codes, this is later used to determine the correct probe function.
			 */
			DEBUG_WARN(
				"Patching Designer code 0x%03" PRIx16 " -> 0x%03" PRIx16 "\n", designer_code, JEP106_MANUFACTURER_STM);
			designer_code = JEP106_MANUFACTURER_STM;
		}
	} else {
		/* legacy ascii code */
		designer_code = (pidr & PIDR_JEP106_CODE_MASK) >> PIDR_JEP106_CODE_OFFSET | ASCII_CODE_FLAG;
	}

	/* Extract part number from the part id register. */
	const uint16_t part_number = pidr & PIDR_PN_MASK;

	/* ROM table */
	if (cid_class == cidc_romtab) {
		if (recursion == 0) {
			ap->designer_code = designer_code;
			ap->partno = part_number;

			if (ap->designer_code == JEP106_MANUFACTURER_ATMEL && ap->partno == 0xcd0) {
				uint32_t ctrlstat = adiv5_mem_read32(ap, SAMX5X_DSU_CTRLSTAT);
				if (ctrlstat & SAMX5X_STATUSB_PROT) {
					/* A protected SAMx5x device is found.
					 * Handle it here, as access only to limited memory region
					 * is allowed
					 */
					cortexm_probe(ap);
					return;
				}
			}
		}

#if defined(ENABLE_DEBUG) && defined(PLATFORM_HAS_DEBUG)
		/* Check SYSMEM bit */
		const uint32_t memtype = adiv5_mem_read32(ap, addr | ADIV5_ROM_MEMTYPE) & ADIV5_ROM_MEMTYPE_SYSMEM;

		if (adiv5_dp_error(ap->dp)) {
			DEBUG_WARN("Fault reading ROM table entry\n");
		}

		DEBUG_INFO("ROM: Table BASE=0x%" PRIx32 " SYSMEM=0x%08" PRIx32 ", Manufacturer %3x Partno %3x\n", addr, memtype,
			designer_code, part_number);
#endif
		for (size_t i = 0; i < 960; i++) {
			adiv5_dp_error(ap->dp);

			uint32_t entry = adiv5_mem_read32(ap, addr + i * 4);
			if (adiv5_dp_error(ap->dp)) {
				DEBUG_WARN("%sFault reading ROM table entry %d\n", indent, i);
				break;
			}

			if (entry == 0)
				break;

			if (!(entry & ADIV5_ROM_ROMENTRY_PRESENT)) {
				DEBUG_INFO("%s%d Entry 0x%" PRIx32 " -> Not present\n", indent, i, entry);
				continue;
			}

			/* Probe recursively */
			adiv5_component_probe(ap, addr + (entry & ADIV5_ROM_ROMENTRY_OFFSET), recursion + 1, i);
		}
		DEBUG_INFO("%sROM: Table END\n", indent);

	} else {
		if (designer_code != JEP106_MANUFACTURER_ARM) {
			/* non arm components not supported currently */
			DEBUG_WARN("%s0x%" PRIx32 ": 0x%08" PRIx32 "%08" PRIx32 " Non ARM component ignored\n", indent, addr,
				(uint32_t)(pidr >> 32U), (uint32_t)pidr);
			return;
		}

		/* ADIv6: For CoreSight components, read DEVTYPE and ARCHID */
		uint16_t arch_id = 0;
		uint8_t dev_type = 0;
		if (cid_class == cidc_dc) {
			dev_type = adiv5_mem_read32(ap, addr + DEVTYPE_OFFSET) & DEVTYPE_MASK;

			uint32_t devarch = adiv5_mem_read32(ap, addr + DEVARCH_OFFSET);

			if (devarch & DEVARCH_PRESENT) {
				arch_id = devarch & DEVARCH_ARCHID_MASK;
			}
		}

		/* Find the part number in our part list and run the appropriate probe routine if applicable. */
		size_t i;
		for (i = 0; arm_component_lut[i].arch != aa_end; i++) {
			if (arm_component_lut[i].part_number != part_number || arm_component_lut[i].dev_type != dev_type ||
				arm_component_lut[i].arch_id != arch_id)
				continue;

			DEBUG_INFO("%s%" PRIu32 " 0x%" PRIx32 ": %s - %s %s (PIDR = 0x%08" PRIx32 "%08" PRIx32 "  DEVTYPE = 0x%02x "
					   "ARCHID = 0x%04x)\n",
				indent + 1, num_entry, addr, cidc_debug_strings[cid_class], arm_component_lut[i].type,
				arm_component_lut[i].full, (uint32_t)(pidr >> 32U), (uint32_t)pidr, dev_type, arch_id);

			/* Perform sanity check, if we know what to expect as * component ID class. */
			if (arm_component_lut[i].cidc != cidc_unknown && cid_class != arm_component_lut[i].cidc) {
				DEBUG_WARN("%sWARNING: \"%s\" !match expected \"%s\"\n", indent + 1, cidc_debug_strings[cid_class],
					cidc_debug_strings[arm_component_lut[i].cidc]);
			}

			switch (arm_component_lut[i].arch) {
			case aa_cortexm:
				DEBUG_INFO("%s-> cortexm_probe\n", indent + 1);
				cortexm_probe(ap);
				break;
			case aa_cortexa:
				DEBUG_INFO("%s-> cortexa_probe\n", indent + 1);
				cortexa_probe(ap, addr);
				break;
			default:
				break;
			}
			break;
		}
		if (arm_component_lut[i].arch == aa_end) {
			DEBUG_WARN("%s%" PRIu32 " 0x%" PRIx32 ": %s - Unknown (PIDR = 0x%08" PRIx32 "%08" PRIx32 " DEVTYPE = "
					   "0x%02x ARCHID = "
					   "0x%04x)\n",
				indent, num_entry, addr, cidc_debug_strings[cid_class], (uint32_t)(pidr >> 32U), (uint32_t)pidr,
				dev_type, arch_id);
		}
	}
}

ADIv5_AP_t *adiv5_new_ap(ADIv5_DP_t *dp, uint8_t apsel)
{
	ADIv5_AP_t *ap, tmpap;
	/* Assume valid and try to read IDR */
	memset(&tmpap, 0, sizeof(tmpap));
	tmpap.dp = dp;
	tmpap.apsel = apsel;
	tmpap.idr = adiv5_ap_read(&tmpap, ADIV5_AP_IDR);
	tmpap.base = adiv5_ap_read(&tmpap, ADIV5_AP_BASE);
	/* Check the Debug Base Address register. See ADIv5
		 * Specification C2.6.1 */
	if (tmpap.base == 0xffffffff) {
		/* Debug Base Address not present in this MEM-AP */
		/* No debug entries... useless AP */
		/* AP0 on STM32MP157C reads 0x00000002 */
		return NULL;
	}

	if (!tmpap.idr) /* IDR Invalid */
		return NULL;
	tmpap.csw = adiv5_ap_read(&tmpap, ADIV5_AP_CSW) & ~(ADIV5_AP_CSW_SIZE_MASK | ADIV5_AP_CSW_ADDRINC_MASK);

	if (tmpap.csw & ADIV5_AP_CSW_TRINPROG) {
		DEBUG_WARN("AP %d: Transaction in progress. AP is not usable!\n", apsel);
		return NULL;
	}

	/* It's valid to so create a heap copy */
	ap = malloc(sizeof(*ap));
	if (!ap) { /* malloc failed: heap exhaustion */
		DEBUG_WARN("malloc: failed in %s\n", __func__);
		return NULL;
	}

	memcpy(ap, &tmpap, sizeof(*ap));

#if defined(ENABLE_DEBUG)
	uint32_t cfg = adiv5_ap_read(ap, ADIV5_AP_CFG);
	DEBUG_INFO("AP %3d: IDR=%08" PRIx32 " CFG=%08" PRIx32 " BASE=%08" PRIx32 " CSW=%08" PRIx32, apsel, ap->idr, cfg,
		ap->base, ap->csw);
	DEBUG_INFO(" (AHB-AP var%" PRIx32 " rev%" PRIx32 ")\n", (ap->idr >> 4) & 0xf, ap->idr >> 28);
#endif
	adiv5_ap_ref(ap);
	return ap;
}

/* No real AP on RP2040. Special setup.*/
static void rp_rescue_setup(ADIv5_DP_t *dp)
{
	ADIv5_AP_t *ap = calloc(1, sizeof(*ap));
	if (!ap) { /* calloc failed: heap exhaustion */
		DEBUG_WARN("calloc: failed in %s\n", __func__);
		return;
	}
	ap->dp = dp;

	rp_rescue_probe(ap);
	return;
}

void adiv5_dp_init(ADIv5_DP_t *dp, const uint32_t idcode)
{
	/*
	 * Assume DP v1 or later.
	 * this may not be true for JTAG-DP
	 * in such cases (DPv0) DPIDR is not implemented
	 * and reads are UNPREDICTABLE.
	 *
	 * for SWD-DP, we are guaranteed to be DP v1 or later.
	 */
	volatile uint32_t dpidr = 0;
	volatile struct exception e;
	TRY_CATCH (e, EXCEPTION_ALL) {
		if (idcode != JTAG_IDCODE_ARM_DPv0)
			dpidr = adiv5_dp_read(dp, ADIV5_DP_DPIDR);
	}
	if (e.type) {
		DEBUG_WARN("DP not responding!...\n");
		free(dp);
		return;
	}

	dp->version = (dpidr & ADIV5_DP_DPIDR_VERSION_MASK) >> ADIV5_DP_DPIDR_VERSION_OFFSET;
	if (dp->version > 0 && (dpidr & 1U)) {
		/*
		* the code in the DPIDR is in the form
		* Bits 10:7 - JEP-106 Continuation code
		* Bits 6:0 - JEP-106 Identity code
		* here we convert it to our internal representation, See JEP-106 code list
		*
		* note: this is the code of the designer not the implementer, we expect it to be ARM
		*/
		const uint16_t designer = (dpidr & ADIV5_DP_DPIDR_DESIGNER_MASK) >> ADIV5_DP_DPIDR_DESIGNER_OFFSET;
		dp->designer_code =
			(designer & ADIV5_DP_DESIGNER_JEP106_CONT_MASK) << 1U | (designer & ADIV5_DP_DESIGNER_JEP106_CODE_MASK);
		dp->partno = (dpidr & ADIV5_DP_DPIDR_PARTNO_MASK) >> ADIV5_DP_DPIDR_PARTNO_OFFSET;

		dp->mindp = !!(dpidr & ADIV5_DP_DPIDR_MINDP);

		/* Check for a valid DPIDR / designer */
		if (dp->designer_code != 0) {
			DEBUG_INFO("DP DPIDR 0x%08" PRIx32 " (v%x %srev%"PRId32") designer 0x%x partno 0x%x\n", dpidr,
				dp->version, dp->mindp ? "MINDP " : "",
				(dpidr & ADIV5_DP_DPIDR_REVISION_MASK) >> ADIV5_DP_DPIDR_REVISION_OFFSET, dp->designer_code,
				dp->partno);
		} else {
			DEBUG_WARN("Invalid DPIDR %08" PRIx32 " assuming DP version 0\n", dpidr);
			dp->version = 0;
			dp->designer_code = 0;
			dp->partno = 0;
			dp->mindp = false;
		}
	} else if (dp->version == 0)
		/* DP v0 */
		DEBUG_WARN("DPv0 detected based on JTAG IDCode\n");

	if (dp->version >= 2) {
		adiv5_dp_write(dp, ADIV5_DP_SELECT, 2); /* TARGETID is on bank 2 */
		const uint32_t targetid = adiv5_dp_read(dp, ADIV5_DP_TARGETID);
		adiv5_dp_write(dp, ADIV5_DP_SELECT, 0);

		/* Use TARGETID register to identify target */
		const uint16_t tdesigner = (targetid & ADIV5_DP_TARGETID_TDESIGNER_MASK) >> ADIV5_DP_TARGETID_TDESIGNER_OFFSET;

		/* convert it to our internal representation, See JEP-106 code list */
		dp->target_designer_code =
			(tdesigner & ADIV5_DP_DESIGNER_JEP106_CONT_MASK) << 1U | (tdesigner & ADIV5_DP_DESIGNER_JEP106_CODE_MASK);

		dp->target_partno = (targetid & ADIV5_DP_TARGETID_TPARTNO_MASK) >> ADIV5_DP_TARGETID_TPARTNO_OFFSET;

		DEBUG_INFO("TARGETID 0x%08" PRIx32 " designer 0x%x partno 0x%x\n", targetid,
			dp->target_designer_code, dp->target_partno);

		dp->targetsel = dp->instance << ADIV5_DP_TARGETSEL_TINSTANCE_OFFSET |
		                (targetid & (ADIV5_DP_TARGETID_TDESIGNER_MASK | ADIV5_DP_TARGETID_TPARTNO_MASK)) | 1U;
	}

	if (dp->designer_code == JEP106_MANUFACTURER_RASPBERRY && dp->partno == 0x2) {
		rp_rescue_setup(dp);
		return;
	}

#if PC_HOSTED == 1
	platform_adiv5_dp_defaults(dp);
	if (!dp->ap_write)
		dp->ap_write = firmware_ap_write;
	if (!dp->ap_read)
		dp->ap_read = firmware_ap_read;
	if (!dp->mem_read)
		dp->mem_read = firmware_mem_read;
	if (!dp->mem_write_sized)
		dp->mem_write_sized = firmware_mem_write_sized;
#else
	dp->ap_write = firmware_ap_write;
	dp->ap_read = firmware_ap_read;
	dp->mem_read = firmware_mem_read;
	dp->mem_write_sized = firmware_mem_write_sized;
#endif

	volatile uint32_t ctrlstat = 0;
	TRY_CATCH (e, EXCEPTION_TIMEOUT) {
		ctrlstat = adiv5_dp_read(dp, ADIV5_DP_CTRLSTAT);
	}
	if (e.type) {
		DEBUG_WARN("DP not responding!  Trying abort sequence...\n");
		adiv5_dp_abort(dp, ADIV5_DP_ABORT_DAPABORT);
		ctrlstat = adiv5_dp_read(dp, ADIV5_DP_CTRLSTAT);
	}

	platform_timeout timeout;
	platform_timeout_set(&timeout, 201);
	/* Write request for system and debug power up */
	adiv5_dp_write(dp, ADIV5_DP_CTRLSTAT, ctrlstat |= ADIV5_DP_CTRLSTAT_CSYSPWRUPREQ | ADIV5_DP_CTRLSTAT_CDBGPWRUPREQ);
	/* Wait for acknowledge */
	while (1) {
		ctrlstat = adiv5_dp_read(dp, ADIV5_DP_CTRLSTAT);
		uint32_t check = ctrlstat & (ADIV5_DP_CTRLSTAT_CSYSPWRUPACK | ADIV5_DP_CTRLSTAT_CDBGPWRUPACK);
		if (check == (ADIV5_DP_CTRLSTAT_CSYSPWRUPACK | ADIV5_DP_CTRLSTAT_CDBGPWRUPACK))
			break;
		if (platform_timeout_is_expired(&timeout)) {
			DEBUG_INFO("DEBUG Power-Up failed\n");
			free(dp); /* No AP that referenced this DP so long*/
			return;
		}
	}
	/* This AP reset logic is described in ADIv5, but fails to work
	 * correctly on STM32.	CDBGRSTACK is never asserted, and we
	 * just wait forever.  This scenario is described in B2.4.1
	 * so we have a timeout mechanism in addition to the sensing one.
	 *
	 * Write request for debug reset */
	adiv5_dp_write(dp, ADIV5_DP_CTRLSTAT, ctrlstat |= ADIV5_DP_CTRLSTAT_CDBGRSTREQ);

	/* Write request for debug reset release */
	adiv5_dp_write(dp, ADIV5_DP_CTRLSTAT, ctrlstat &= ~ADIV5_DP_CTRLSTAT_CDBGRSTREQ);
	/* Wait for acknowledge */
	while (1) {
		platform_delay(20);
		ctrlstat = adiv5_dp_read(dp, ADIV5_DP_CTRLSTAT);
		if (ctrlstat & ADIV5_DP_CTRLSTAT_CDBGRSTACK) {
			DEBUG_INFO("RESET_SEQ succeeded.\n");
			break;
		}
		if (platform_timeout_is_expired(&timeout)) {
			DEBUG_INFO("RESET_SEQ failed\n");
			break;
		}
	}

	/* Probe for APs on this DP */
	uint32_t last_base = 0;
	size_t invalid_aps = 0;
	dp->refcnt++;
	for (size_t i = 0; i < 256 && invalid_aps < 8; ++i) {
		ADIv5_AP_t *ap = NULL;
#if PC_HOSTED == 1
		if ((!dp->ap_setup) || dp->ap_setup(i))
			ap = adiv5_new_ap(dp, i);
#else
		ap = adiv5_new_ap(dp, i);
#endif
		if (ap == NULL) {
#if PC_HOSTED == 1
			if (dp->ap_cleanup)
				dp->ap_cleanup(i);
#endif
			if (++invalid_aps == 8) {
				adiv5_dp_unref(dp);
				return;
			}
			continue;
		}
		if (ap->base == last_base) {
			DEBUG_WARN("AP %d: Duplicate base\n", i);
#if PC_HOSTED == 1
			if (dp->ap_cleanup)
				dp->ap_cleanup(i);
#endif
			adiv5_ap_unref(ap);
			adiv5_dp_unref(dp);
			/* FIXME: Should we expect valid APs behind duplicate ones? */
			return;
		}
		last_base = ap->base;

		kinetis_mdm_probe(ap);
		nrf51_mdm_probe(ap);
		efm32_aap_probe(ap);

		/* Halt the device and release from reset if reset is active!*/
		if (!ap->apsel && ((ap->idr & 0xf) == ARM_AP_TYPE_AHB))
			cortexm_prepare(ap);
		/* Should probe further here to make sure it's a valid target.
		 * AP should be unref'd if not valid.
		 */

		/* The rest should only be added after checking ROM table */
		adiv5_component_probe(ap, ap->base, 0, 0);
		adiv5_ap_unref(ap);
	}
	/* We halted at least CortexM for Romtable scan.
	 * With connect under reset, keep the devices halted.
	 * Otherwise, release the devices now.
	 * Attach() will halt them again.
	 */
	for (target *t = target_list; t; t = t->next) {
		if (!connect_assert_nrst) {
			target_halt_resume(t, false);
		}
	}
	adiv5_dp_unref(dp);
}

#define ALIGNOF(x) (((x)&3) == 0 ? ALIGN_WORD : (((x)&1) == 0 ? ALIGN_HALFWORD : ALIGN_BYTE))

/* Program the CSW and TAR for sequencial access at a given width */
static void ap_mem_access_setup(ADIv5_AP_t *ap, uint32_t addr, enum align align)
{
	uint32_t csw = ap->csw | ADIV5_AP_CSW_ADDRINC_SINGLE;

	switch (align) {
	case ALIGN_BYTE:
		csw |= ADIV5_AP_CSW_SIZE_BYTE;
		break;
	case ALIGN_HALFWORD:
		csw |= ADIV5_AP_CSW_SIZE_HALFWORD;
		break;
	case ALIGN_DWORD:
	case ALIGN_WORD:
		csw |= ADIV5_AP_CSW_SIZE_WORD;
		break;
	}
	adiv5_ap_write(ap, ADIV5_AP_CSW, csw);
	adiv5_dp_low_access(ap->dp, ADIV5_LOW_WRITE, ADIV5_AP_TAR, addr);
}

/* Extract read data from data lane based on align and src address */
void *extract(void *dest, uint32_t src, uint32_t val, enum align align)
{
	switch (align) {
	case ALIGN_BYTE:
		*(uint8_t *)dest = (val >> ((src & 0x3) << 3) & 0xFF);
		break;
	case ALIGN_HALFWORD:
		*(uint16_t *)dest = (val >> ((src & 0x2) << 3) & 0xFFFF);
		break;
	case ALIGN_DWORD:
	case ALIGN_WORD:
		*(uint32_t *)dest = val;
		break;
	}
	return (uint8_t *)dest + (1 << align);
}

void firmware_mem_read(ADIv5_AP_t *ap, void *dest, uint32_t src, size_t len)
{
	uint32_t tmp;
	uint32_t osrc = src;
	const enum align align = MIN(ALIGNOF(src), ALIGNOF(len));

	if (len == 0)
		return;

	len >>= align;
	ap_mem_access_setup(ap, src, align);
	adiv5_dp_low_access(ap->dp, ADIV5_LOW_READ, ADIV5_AP_DRW, 0);
	while (--len) {
		tmp = adiv5_dp_low_access(ap->dp, ADIV5_LOW_READ, ADIV5_AP_DRW, 0);
		dest = extract(dest, src, tmp, align);

		src += (1U << align);
		/* Check for 10 bit address overflow */
		if ((src ^ osrc) & 0xfffffc00U) {
			osrc = src;
			adiv5_dp_low_access(ap->dp, ADIV5_LOW_WRITE, ADIV5_AP_TAR, src);
			adiv5_dp_low_access(ap->dp, ADIV5_LOW_READ, ADIV5_AP_DRW, 0);
		}
	}
	tmp = adiv5_dp_low_access(ap->dp, ADIV5_LOW_READ, ADIV5_DP_RDBUFF, 0);
	extract(dest, src, tmp, align);
}

void firmware_mem_write_sized(ADIv5_AP_t *ap, uint32_t dest, const void *src, size_t len, enum align align)
{
	uint32_t odest = dest;

	len >>= align;
	ap_mem_access_setup(ap, dest, align);
	while (len--) {
		uint32_t tmp = 0;
		/* Pack data into correct data lane */
		switch (align) {
		case ALIGN_BYTE:
			tmp = ((uint32_t) * (uint8_t *)src) << ((dest & 3) << 3);
			break;
		case ALIGN_HALFWORD:
			tmp = ((uint32_t) * (uint16_t *)src) << ((dest & 2) << 3);
			break;
		case ALIGN_DWORD:
		case ALIGN_WORD:
			tmp = *(uint32_t *)src;
			break;
		}
		src = (uint8_t *)src + (1 << align);
		dest += (1 << align);
		adiv5_dp_low_access(ap->dp, ADIV5_LOW_WRITE, ADIV5_AP_DRW, tmp);

		/* Check for 10 bit address overflow */
		if ((dest ^ odest) & 0xfffffc00U) {
			odest = dest;
			adiv5_dp_low_access(ap->dp, ADIV5_LOW_WRITE, ADIV5_AP_TAR, dest);
		}
	}
	/* Make sure this write is complete by doing a dummy read */
	adiv5_dp_read(ap->dp, ADIV5_DP_RDBUFF);
}

void firmware_ap_write(ADIv5_AP_t *ap, uint16_t addr, uint32_t value)
{
	adiv5_dp_write(ap->dp, ADIV5_DP_SELECT, ((uint32_t)ap->apsel << 24) | (addr & 0xF0));
	adiv5_dp_write(ap->dp, addr, value);
}

uint32_t firmware_ap_read(ADIv5_AP_t *ap, uint16_t addr)
{
	uint32_t ret;
	adiv5_dp_write(ap->dp, ADIV5_DP_SELECT, ((uint32_t)ap->apsel << 24) | (addr & 0xF0));
	ret = adiv5_dp_read(ap->dp, addr);
	return ret;
}

void adiv5_mem_write(ADIv5_AP_t *ap, uint32_t dest, const void *src, size_t len)
{
	enum align align = MIN(ALIGNOF(dest), ALIGNOF(len));
	adiv5_mem_write_sized(ap, dest, src, len, align);
}
