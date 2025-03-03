/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2014  Richard Meadows <richardeoin>
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

/* This file implements Atmel SAM D target specific functions for
 * detecting the device, providing the XML memory map and Flash memory
 * programming.
 *
 * Tested with
 * * SAMD09D14A (rev B)
 * * SAMD20E17A (rev C)
 * * SAMD20J18A (rev B)
 * * SAMD21J18A (rev B)
 * * SAML21J17B (rev B)
 * *
 */
/* Refer to the SAM D20 Datasheet:
 * http://www.atmel.com/Images/Atmel-42129-SAM-D20_Datasheet.pdf
 * particularly Sections 12. DSU and 20. NVMCTRL
 */

#include "general.h"
#include "target.h"
#include "target_internal.h"
#include "cortexm.h"

static int samd_flash_erase(struct target_flash *f, target_addr addr, size_t len);
static int samd_flash_write(struct target_flash *f, target_addr dest, const void *src, size_t len);
bool samd_mass_erase(target *t);

static bool samd_cmd_lock_flash(target *t, int argc, const char **argv);
static bool samd_cmd_unlock_flash(target *t, int argc, const char **argv);
static bool samd_cmd_unlock_bootprot(target *t, int argc, const char **argv);
static bool samd_cmd_lock_bootprot(target *t, int argc, const char **argv);
static bool samd_cmd_read_userrow(target *t, int argc, const char **argv);
static bool samd_cmd_serial(target *t, int argc, const char **argv);
static bool samd_cmd_mbist(target *t, int argc, const char **argv);
static bool samd_cmd_ssb(target *t, int argc, const char **argv);

const struct command_s samd_cmd_list[] = {
	{"lock_flash", samd_cmd_lock_flash, "Locks flash against spurious commands"},
	{"unlock_flash", samd_cmd_unlock_flash, "Unlocks flash"},
	{"lock_bootprot", samd_cmd_lock_bootprot, "Lock the boot protections to maximum"},
	{"unlock_bootprot", samd_cmd_unlock_bootprot, "Unlock the boot protections to minimum"},
	{"user_row", samd_cmd_read_userrow, "Prints user row from flash"},
	{"serial", samd_cmd_serial, "Prints serial number"},
	{"mbist", samd_cmd_mbist, "Runs the built-in memory test"},
	{"set_security_bit", samd_cmd_ssb, "Sets the Security Bit"},
	{NULL, NULL, NULL}
};

/* Non-Volatile Memory Controller (NVMC) Parameters */
#define SAMD_ROW_SIZE  256U
#define SAMD_PAGE_SIZE 64U

/* -------------------------------------------------------------------------- */
/* Non-Volatile Memory Controller (NVMC) Registers */
/* -------------------------------------------------------------------------- */

#define SAMD_NVMC         0x41004000U
#define SAMD_NVMC_CTRLA   (SAMD_NVMC + 0x00U)
#define SAMD_NVMC_CTRLB   (SAMD_NVMC + 0x04U)
#define SAMD_NVMC_PARAM   (SAMD_NVMC + 0x08U)
#define SAMD_NVMC_INTFLAG (SAMD_NVMC + 0x14U)
#define SAMD_NVMC_STATUS  (SAMD_NVMC + 0x18U)
#define SAMD_NVMC_ADDRESS (SAMD_NVMC + 0x1CU)

/* Control A Register (CTRLA) */
#define SAMD_CTRLA_CMD_KEY             0xA500U
#define SAMD_CTRLA_CMD_ERASEROW        0x0002U
#define SAMD_CTRLA_CMD_WRITEPAGE       0x0004U
#define SAMD_CTRLA_CMD_ERASEAUXROW     0x0005U
#define SAMD_CTRLA_CMD_WRITEAUXPAGE    0x0006U
#define SAMD_CTRLA_CMD_LOCK            0x0040U
#define SAMD_CTRLA_CMD_UNLOCK          0x0041U
#define SAMD_CTRLA_CMD_PAGEBUFFERCLEAR 0x0044U
#define SAMD_CTRLA_CMD_SSB             0x0045U
#define SAMD_CTRLA_CMD_INVALL          0x0046U

/* Interrupt Flag Register (INTFLAG) */
#define SAMD_NVMC_READY (1U << 0U)

/* Non-Volatile Memory Calibration and Auxiliary Registers */
#define SAMD_NVM_USER_ROW_LOW  0x00804000U
#define SAMD_NVM_USER_ROW_HIGH 0x00804004U
#define SAMD_NVM_CALIBRATION   0x00806020U
#define SAMD_NVM_SERIAL(n)     (0x0080A00CU + (0x30U * (((n) + 3U) / 4U)) + ((n) * 4U))

/* -------------------------------------------------------------------------- */
/* Device Service Unit (DSU) Registers */
/* -------------------------------------------------------------------------- */

#define SAMD_DSU            0x41002000U
#define SAMD_DSU_EXT_ACCESS (SAMD_DSU + 0x100U)
#define SAMD_DSU_CTRLSTAT   (SAMD_DSU_EXT_ACCESS + 0x0U)
#define SAMD_DSU_ADDRESS    (SAMD_DSU_EXT_ACCESS + 0x4U)
#define SAMD_DSU_LENGTH     (SAMD_DSU_EXT_ACCESS + 0x8U)
#define SAMD_DSU_DID        (SAMD_DSU_EXT_ACCESS + 0x018U)
#define SAMD_DSU_PID        (SAMD_DSU + 0x1000U)
#define SAMD_DSU_CID        (SAMD_DSU + 0x1010U)

/* Control and Status Register (CTRLSTAT) */
#define SAMD_CTRL_CHIP_ERASE (1U << 4U)
#define SAMD_CTRL_MBIST      (1U << 3U)
#define SAMD_CTRL_CRC        (1U << 2U)
#define SAMD_STATUSA_PERR    (1U << 12U)
#define SAMD_STATUSA_FAIL    (1U << 11U)
#define SAMD_STATUSA_BERR    (1U << 10U)
#define SAMD_STATUSA_CRSTEXT (1U << 9U)
#define SAMD_STATUSA_DONE    (1U << 8U)
#define SAMD_STATUSB_PROT    (1U << 16U)

/* Device Identification Register (DID) */
#define SAMD_DID_MASK          0xFF380000U
#define SAMD_DID_CONST_VALUE   0x10000000U
#define SAMD_DID_DEVSEL_MASK   0xFFU
#define SAMD_DID_DEVSEL_POS    0U
#define SAMD_DID_REVISION_MASK 0x0FU
#define SAMD_DID_REVISION_POS  8U
#define SAMD_DID_SERIES_MASK   0x1FU
#define SAMD_DID_SERIES_POS    16U
#define SAMD_DID_FAMILY_MASK   0x3FU
#define SAMD_DID_FAMILY_POS    23U

/* Peripheral ID */
#define SAMD_PID_MASK        0x00F7FFFFU
#define SAMD_PID_CONST_VALUE 0x0001FCD0U

/* Component ID */
#define SAMD_CID_VALUE 0xB105100DU

/* Family parts */
struct samd_part {
	uint8_t devsel;
	char pin;
	uint8_t mem;
	uint8_t variant;
};

static const struct samd_part samd_d21_parts[] = {
	{0x00, 'J', 18, 'A'}, /* SAMD21J18A */
	{0x01, 'J', 17, 'A'}, /* SAMD21J17A */
	{0x02, 'J', 16, 'A'}, /* SAMD21J16A */
	{0x03, 'J', 15, 'A'}, /* SAMD21J15A */
	{0x05, 'G', 18, 'A'}, /* SAMD21G18A */
	{0x06, 'G', 17, 'A'}, /* SAMD21G17A */
	{0x07, 'G', 16, 'A'}, /* SAMD21G16A */
	{0x08, 'G', 15, 'A'}, /* SAMD21G15A */
	{0x0A, 'E', 18, 'A'}, /* SAMD21E18A */
	{0x0B, 'E', 17, 'A'}, /* SAMD21E17A */
	{0x0C, 'E', 16, 'A'}, /* SAMD21E16A */
	{0x0D, 'E', 15, 'A'}, /* SAMD21E15A */
	{0x0F, 'G', 18, 'A'}, /* SAMD21G18A (WLCSP) */
	{0x10, 'G', 17, 'A'}, /* SAMD21G17A (WLCSP) */
	{0x20, 'J', 16, 'B'}, /* SAMD21J16B */
	{0x21, 'J', 15, 'B'}, /* SAMD21J15B */
	{0x23, 'G', 16, 'B'}, /* SAMD21G16B */
	{0x24, 'G', 15, 'B'}, /* SAMD21G15B */
	{0x26, 'E', 16, 'B'}, /* SAMD21E16B */
	{0x27, 'E', 15, 'B'}, /* SAMD21E15B */
	{0x55, 'E', 16, 'B'}, /* SAMD21E16B (WLCSP) */
	{0x56, 'E', 15, 'B'}, /* SAMD21E15B (WLCSP) */
	{0x62, 'E', 16, 'C'}, /* SAMD21E16C (WLCSP) */
	{0x63, 'E', 15, 'C'}, /* SAMD21E15C (WLCSP) */
	{0xFF, 0, 0, 0}
};

static const struct samd_part samd_l21_parts[] = {
	{0x00, 'J', 18, 'A'}, /* SAML21J18A */
	{0x01, 'J', 17, 'A'}, /* SAML21J17A */
	{0x02, 'J', 16, 'A'}, /* SAML21J16A */
	{0x05, 'G', 18, 'A'}, /* SAML21G18A */
	{0x06, 'G', 17, 'A'}, /* SAML21G17A */
	{0x07, 'G', 16, 'A'}, /* SAML21G16A */
	{0x0A, 'E', 18, 'A'}, /* SAML21E18A */
	{0x0B, 'E', 17, 'A'}, /* SAML21E17A */
	{0x0C, 'E', 16, 'A'}, /* SAML21E16A */
	{0x0D, 'E', 15, 'A'}, /* SAML21E15A */
	{0x0F, 'J', 18, 'B'}, /* SAML21J18B */
	{0x10, 'J', 17, 'B'}, /* SAML21J17B */
	{0x11, 'J', 16, 'B'}, /* SAML21J16B */
	{0x14, 'G', 18, 'B'}, /* SAML21G18B */
	{0x15, 'G', 17, 'B'}, /* SAML21G17B */
	{0x16, 'G', 16, 'B'}, /* SAML21G16B */
	{0x19, 'E', 18, 'B'}, /* SAML21E18B */
	{0x1A, 'E', 17, 'B'}, /* SAML21E17B */
	{0x1B, 'E', 16, 'B'}, /* SAML21E16B */
	{0x1C, 'E', 15, 'B'}, /* SAML21E15B */
	{0xFF, 0, 0, 0}
};

static const struct samd_part samd_l22_parts[] = {
	{0x00, 'N',	18,	'A'}, /* SAML22N18 */
	{0x01, 'N',	17,	'A'}, /* SAML22N17 */
	{0x02, 'N',	16,	'A'}, /* SAML22N16 */
	{0x05, 'J',	18,	'A'}, /* SAML22J18 */
	{0x06, 'J',	17,	'A'}, /* SAML22J17 */
	{0x07, 'J',	16,	'A'}, /* SAML22J16 */
	{0x0A, 'G',	18,	'A'}, /* SAML22G18 */
	{0x0B, 'G',	17,	'A'}, /* SAML22G17 */
	{0x0C, 'G',	16,	'A'}, /* SAML22G16 */
	{0xFF, 0, 0, 0}
};

/*
 * Overloads the default cortexm reset function with a version that
 * removes the target from extended reset where required.
 */
void samd_reset(target *t)
{
	/*
	 * nRST is not asserted here as it appears to reset the adiv5
	 * logic, meaning that subsequent adiv5_* calls PLATFORM_FATAL_ERROR.
	 *
	 * This is ok as normally you can just connect the debugger and go,
	 * but if that's not possible (protection or SWCLK being used for
	 * something else) then having SWCLK low on reset should get you
	 * debug access (cold-plugging). TODO: Confirm this
	 *
	 * See the SAM D20 datasheet §12.6 Debug Operation for more
	 * details.
	 *
	 * jtagtap_nrst(true);
	 * jtagtap_nrst(false);
	 */

	/* Read DHCSR here to clear S_RESET_ST bit before reset */
	target_mem_read32(t, CORTEXM_DHCSR);

	/*
	 * Request System Reset from NVIC: nRST doesn't work correctly
	 * This could be VECTRESET: 0x05FA0001 (reset only core)
	 *          or SYSRESETREQ: 0x05FA0004 (system reset)
	 */
	target_mem_write32(t, CORTEXM_AIRCR,
	                   CORTEXM_AIRCR_VECTKEY | CORTEXM_AIRCR_SYSRESETREQ);

	/* Exit extended reset */
	if (target_mem_read32(t, SAMD_DSU_CTRLSTAT) & SAMD_STATUSA_CRSTEXT)
		/* Write bit to clear from extended reset */
		target_mem_write32(t, SAMD_DSU_CTRLSTAT, SAMD_STATUSA_CRSTEXT);

	/* Poll for release from reset */
	while (target_mem_read32(t, CORTEXM_DHCSR) & CORTEXM_DHCSR_S_RESET_ST)
		continue;

	/* Reset DFSR flags */
	target_mem_write32(t, CORTEXM_DFSR, CORTEXM_DFSR_RESETALL);

	/* Clear any target errors */
	target_check_error(t);
}

/*
 * Overloads the default cortexm detached function with a version that
 * removes the target from extended reset where required.
 *
 * Only required for SAM D20 _Revision B_ Silicon
 */
static void samd20_revB_detach(target *t)
{
	cortexm_detach(t);

	/* Exit extended reset */
	if (target_mem_read32(t, SAMD_DSU_CTRLSTAT) & SAMD_STATUSA_CRSTEXT)
		/* Write bit to clear from extended reset */
		target_mem_write32(t, SAMD_DSU_CTRLSTAT, SAMD_STATUSA_CRSTEXT);
}

/*
 * Overloads the default cortexm halt_resume function with a version
 * that removes the target from extended reset where required.
 *
 * Only required for SAM D20 _Revision B_ Silicon
 */
static void samd20_revB_halt_resume(target *t, bool step)
{
	target_halt_resume(t, step);

	/* Exit extended reset */
	if (target_mem_read32(t, SAMD_DSU_CTRLSTAT) & SAMD_STATUSA_CRSTEXT)
		/* Write bit to clear from extended reset */
		target_mem_write32(t, SAMD_DSU_CTRLSTAT, SAMD_STATUSA_CRSTEXT);
}

/*
 * Release the target from extended reset before running the normal cortexm_attach routine.
 * This prevents tripping up over errata ref 9905
 *
 * Only required for SAM D11 silicon.
 */
static bool samd11_attach(target *t)
{
	/* Exit extended reset */
	if (target_mem_read32(t, SAMD_DSU_CTRLSTAT) & SAMD_STATUSA_CRSTEXT)
		/* Write bit to clear from extended reset */
		target_mem_write32(t, SAMD_DSU_CTRLSTAT, SAMD_STATUSA_CRSTEXT);

	return cortexm_attach(t);
}

/*
 * Overload the default cortexm attach for when the samd is protected.
 *
 * If the samd is protected then the default cortexm attach will
 * fail as the S_HALT bit in the DHCSR will never go high. This
 * function allows users to attach on a temporary basis so they can
 * rescue the device.
 */
bool samd_protected_attach(target *t)
{
	tc_printf(t, "Attached in protected mode, please issue 'monitor erase_mass' to regain chip access\n");
	/* Patch back in the normal cortexm attach for next time */
	t->attach = cortexm_attach;

	/* Allow attach this time */
	return true;
}

/*
 * Use the DSU Device Indentification Register to populate a struct
 * describing the SAM D device.
 */
struct samd_descr {
	char family;
	uint8_t series;
	char revision;
	char pin;
	uint32_t ram_size;
	uint32_t flash_size;
	uint8_t mem;
	char variant;
	char package[3];
};

struct samd_descr samd_parse_device_id(uint32_t did)
{
	struct samd_descr samd;
	uint8_t i = 0;
	const struct samd_part *parts = samd_d21_parts;
	memset(samd.package, 0, 3);
	samd.ram_size = 0x8000;
	samd.flash_size = 0x40000;

	uint8_t family = (did >> SAMD_DID_FAMILY_POS) & SAMD_DID_FAMILY_MASK;
	uint8_t series = (did >> SAMD_DID_SERIES_POS) & SAMD_DID_SERIES_MASK;
	uint8_t revision = (did >> SAMD_DID_REVISION_POS) & SAMD_DID_REVISION_MASK;
	uint8_t devsel = (did >> SAMD_DID_DEVSEL_POS) & SAMD_DID_DEVSEL_MASK;

	/* Family */
	switch (family) {
		case 0: samd.family = 'D'; break;
		case 1: samd.family = 'L'; parts = samd_l21_parts; break;
		case 2: samd.family = 'C'; break;
	}
	/* Series */
	switch (series) {
		case 0: samd.series = 20; break;
		case 1: samd.series = 21; break;
		case 2:
			if (family == 1) {
				samd.series = 22;
				parts = samd_l22_parts;
			} else {
				samd.series = 10;
			}
			break;
		case 3: samd.series = 11; break;
		case 4: samd.series = 9; break;
		default: samd.series = 0; break;
	}
	/* Revision */
	samd.revision = (char)('A' + revision);

	switch (samd.series) {
	case 20: /* SAM D20 */
		switch (devsel / 5) {
			case 0: samd.pin = 'J'; break;
			case 1: samd.pin = 'G'; break;
			case 2: samd.pin = 'E'; break;
			default: samd.pin = 'u'; break;
		}
		samd.mem = 18 - (devsel % 5);
		samd.variant = 'A';
		break;
	case 21: /* SAM D21/L21 */
	case 22: /* SAM L22 */
		i = 0;
		while (parts[i].devsel != 0xFF) {
			if (parts[i].devsel == devsel) {
				samd.pin = parts[i].pin;
				samd.mem = parts[i].mem;
				samd.variant = parts[i].variant;
				break;
			}
			i++;
		}
		break;
	case 10: /* SAM D10 */
	case 11: /* SAM D11 */
		switch (devsel / 3) {
			case 0: samd.package[0] = 'M'; break;
			case 1: samd.package[0] = 'S'; samd.package[1] = 'S'; break;
		}
		samd.pin = 'D';
		samd.mem = 14 - (devsel % 3);
		samd.variant = 'A';
		break;
	case 9: /* SAM D09 */
		samd.ram_size = 4096;
		switch (devsel) {
			case 0:
				samd.pin = 'D';
				samd.mem = 14;
				samd.flash_size = 16384;
				samd.package[0] = 'M';
				break;
			case 7:
				samd.pin = 'C';
				samd.mem = 13;
				samd.flash_size = 8192;
				break;
		}
		samd.variant = 'A';
		break;
	}

	return samd;
}

static void samd_add_flash(target *t, uint32_t addr, size_t length)
{
	struct target_flash *f = calloc(1, sizeof(*f));
	if (!f) {			/* calloc failed: heap exhaustion */
		DEBUG_WARN("calloc: failed in %s\n", __func__);
		return;
	}

	f->start = addr;
	f->length = length;
	f->blocksize = SAMD_ROW_SIZE;
	f->erase = samd_flash_erase;
	f->write = samd_flash_write;
	f->buf_size = SAMD_PAGE_SIZE;
	target_add_flash(t, f);
}

struct samd_priv_s {
	char samd_variant_string[60];
};


bool samd_probe(target *t)
{
	ADIv5_AP_t *ap = cortexm_ap(t);
	uint32_t cid = adiv5_ap_read_pidr(ap, SAMD_DSU_CID);
	uint32_t pid = adiv5_ap_read_pidr(ap, SAMD_DSU_PID);

	/* Check the ARM Coresight Component and Perhiperal IDs */
	if ((cid != SAMD_CID_VALUE) ||
	    ((pid & SAMD_PID_MASK) != SAMD_PID_CONST_VALUE))
		return false;

	/* Read the Device ID */
	uint32_t did = target_mem_read32(t, SAMD_DSU_DID);

	/* If the Device ID matches */
	if ((did & SAMD_DID_MASK) != SAMD_DID_CONST_VALUE)
		return false;

	t->mass_erase = samd_mass_erase;
	struct samd_priv_s *priv_storage = calloc(1, sizeof(*priv_storage));
	t->target_storage = (void*)priv_storage;

	uint32_t ctrlstat = target_mem_read32(t, SAMD_DSU_CTRLSTAT);
	struct samd_descr samd = samd_parse_device_id(did);

	/* Protected? */
	bool protected = (ctrlstat & SAMD_STATUSB_PROT);

	/* Part String */
	if (protected) {
		sprintf(priv_storage->samd_variant_string,
		        "Atmel SAM%c%02d%c%d%c%s (rev %c) (PROT=1)",
		        samd.family,
		        samd.series, samd.pin, samd.mem,
		        samd.variant,
		        samd.package, samd.revision);
	} else {
		sprintf(priv_storage->samd_variant_string,
		        "Atmel SAM%c%02d%c%d%c%s (rev %c)",
		        samd.family,
		        samd.series, samd.pin, samd.mem,
		        samd.variant,
		        samd.package, samd.revision);
	}

	/* Setup Target */
	t->driver = priv_storage->samd_variant_string;
	t->reset = samd_reset;

	if (samd.series == 20 && samd.revision == 'B') {
		/*
		 * These functions check for an extended reset.
		 * Appears to be related to Errata 35.4.1 ref 12015
		 */
		t->detach      = samd20_revB_detach;
		t->halt_resume = samd20_revB_halt_resume;
	} else if (samd.series == 11) {
		/*
		 * Attach routine that checks for an extended reset and releases it.
		 * This works around Errata 38.2.5 ref 9905
		 */
		t->attach = samd11_attach;
	}

	if (protected) {
		/*
		 * Overload the default cortexm attach
		 * for when the samd is protected.
		 * This function allows users to
		 * attach on a temporary basis so they
		 * can rescue the device.
		 */
		t->attach = samd_protected_attach;
	}

	target_add_ram(t, 0x20000000, samd.ram_size);
	samd_add_flash(t, 0x00000000, samd.flash_size);
	target_add_commands(t, samd_cmd_list, "SAMD");

	/* If we're not in reset here */
	if (!platform_nrst_get_val()) {
		/* We'll have to release the target from
		 * extended reset to make attach possible */
		if (target_mem_read32(t, SAMD_DSU_CTRLSTAT) &
		    SAMD_STATUSA_CRSTEXT) {

			/* Write bit to clear from extended reset */
			target_mem_write32(t, SAMD_DSU_CTRLSTAT,
			                   SAMD_STATUSA_CRSTEXT);
		}
	}

	return true;
}

/*
 * Temporary (until next reset) flash memory locking / unlocking
 */
static void samd_lock_current_address(target *t)
{
	/* Issue the unlock command */
	target_mem_write32(t, SAMD_NVMC_CTRLA,
	                   SAMD_CTRLA_CMD_KEY | SAMD_CTRLA_CMD_LOCK);
}
static void samd_unlock_current_address(target *t)
{
	/* Issue the unlock command */
	target_mem_write32(t, SAMD_NVMC_CTRLA,
	                   SAMD_CTRLA_CMD_KEY | SAMD_CTRLA_CMD_UNLOCK);
}

/*
 * Erase flash row by row
 */
static int samd_flash_erase(struct target_flash *f, target_addr addr, size_t len)
{
	target *t = f->t;
	while (len) {
		/* Write address of first word in row to erase it */
		/* Must be shifted right for 16-bit address, see Datasheet §20.8.8 Address */
		target_mem_write32(t, SAMD_NVMC_ADDRESS, addr >> 1);

		/* Unlock */
		samd_unlock_current_address(t);

		/* Issue the erase command */
		target_mem_write32(t, SAMD_NVMC_CTRLA,
		                   SAMD_CTRLA_CMD_KEY | SAMD_CTRLA_CMD_ERASEROW);
		/* Poll for NVM Ready */
		while ((target_mem_read32(t, SAMD_NVMC_INTFLAG) & SAMD_NVMC_READY) == 0)
			if (target_check_error(t))
				return -1;

		/* Lock */
		samd_lock_current_address(t);

		addr += f->blocksize;
		if (len > f->blocksize)
			len -= f->blocksize;
		else
			len = 0;
	}

	return 0;
}

/*
 * Write flash page by page
 */
static int samd_flash_write(struct target_flash *f,
                            target_addr dest, const void *src, size_t len)
{
	target *t = f->t;

	/* Write within a single page. This may be part or all of the page */
	target_mem_write(t, dest, src, len);

	/* Unlock */
	samd_unlock_current_address(t);

	/* Issue the write page command */
	target_mem_write32(t, SAMD_NVMC_CTRLA,
	                   SAMD_CTRLA_CMD_KEY | SAMD_CTRLA_CMD_WRITEPAGE);

	/* Poll for NVM Ready */
	while ((target_mem_read32(t, SAMD_NVMC_INTFLAG) & SAMD_NVMC_READY) == 0)
		if (target_check_error(t))
			return -1;

	/* Lock */
	samd_lock_current_address(t);

	return 0;
}

/*
 * Uses the Device Service Unit to erase the entire flash
 */
bool samd_mass_erase(target *t)
{
	/* Clear the DSU status bits */
	target_mem_write32(t, SAMD_DSU_CTRLSTAT,
	                   SAMD_STATUSA_DONE | SAMD_STATUSA_PERR |
	                   SAMD_STATUSA_FAIL);

	/* Erase all */
	target_mem_write32(t, SAMD_DSU_CTRLSTAT, SAMD_CTRL_CHIP_ERASE);

	platform_timeout timeout;
	platform_timeout_set(&timeout, 500);
	/* Poll for DSU Ready */
	uint32_t status;
	while (((status = target_mem_read32(t, SAMD_DSU_CTRLSTAT)) &
		(SAMD_STATUSA_DONE | SAMD_STATUSA_PERR | SAMD_STATUSA_FAIL)) == 0) {
		if (target_check_error(t))
			return false;
		target_print_progress(&timeout);
	}

	/* Test the protection error bit in Status A */
	if (status & SAMD_STATUSA_PERR) {
		tc_printf(t, "Erase failed due to a protection error.\n");
		return true;
	}

	/* Test the fail bit in Status A */
	if (status & SAMD_STATUSA_FAIL) {
		tc_printf(t, "Erase failed.\n");
		return true;
	}
	return true;
}

/*
 * Sets the NVM region lock bits in the User Row. This value is read
 * at startup as the default value for the lock bits, and hence does
 * not take effect until a reset.
 *
 * 0x0000 = Lock, 0xFFFF = Unlock (default)
 */
static bool samd_set_flashlock(target *t, uint16_t value, const char **argv)
{
	(void)argv;
	uint32_t high = target_mem_read32(t, SAMD_NVM_USER_ROW_HIGH);
	uint32_t low = target_mem_read32(t, SAMD_NVM_USER_ROW_LOW);

	/* Write address of a word in the row to erase it */
	/* Must be shifted right for 16-bit address, see Datasheet §20.8.8 Address */
	target_mem_write32(t, SAMD_NVMC_ADDRESS, SAMD_NVM_USER_ROW_LOW >> 1);

	/* Issue the erase command */
	target_mem_write32(t, SAMD_NVMC_CTRLA,
	                   SAMD_CTRLA_CMD_KEY | SAMD_CTRLA_CMD_ERASEAUXROW);

	/* Poll for NVM Ready */
	while ((target_mem_read32(t, SAMD_NVMC_INTFLAG) & SAMD_NVMC_READY) == 0)
		if (target_check_error(t))
			return -1;

	/* Modify the high byte of the user row */
	high = (high & 0x0000FFFF) | ((value << 16) & 0xFFFF0000);

	/* Write back */
	target_mem_write32(t, SAMD_NVM_USER_ROW_LOW, low);
	target_mem_write32(t, SAMD_NVM_USER_ROW_HIGH, high);

	/* Issue the page write command */
	target_mem_write32(t, SAMD_NVMC_CTRLA,
	                   SAMD_CTRLA_CMD_KEY | SAMD_CTRLA_CMD_WRITEAUXPAGE);

	return true;
}

static bool parse_unsigned(const char *str, uint32_t *val)
{
	int result;
	unsigned long num;

	size_t len = strlen(str);
	// TODO: port to use substrate::toInt_t<> style parser for robustness and smaller code size
	if (len > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))
		result = sscanf(str + 2, "%lx", &num);
	else
		result = sscanf(str, "%lu", &num);

	if (result < 1)
		return false;

	*val = (uint32_t)num;
	return true;
}

static bool samd_cmd_lock_flash(target *t, int argc, const char **argv)
{
	if (argc > 2) {
		tc_printf(t, "usage: monitor lock_flash [number]\n");
		return false;
	}
	if (argc == 2) {
		uint32_t val = 0;
		if (!parse_unsigned(argv[1], &val)) {
			tc_printf(t, "number must be either decimal or 0x prefixed hexadecimal\n");
			return false;
		}

		if (val > 0xffffU) {
			tc_printf(t, "number must be between 0 and 65535\n");
			return false;
		}

		return samd_set_flashlock(t, (uint16_t)val, NULL);
	}
	return samd_set_flashlock(t, 0x0000, NULL);
}

static bool samd_cmd_unlock_flash(target *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	return samd_set_flashlock(t, 0xFFFF, NULL);
}

static bool samd_set_bootprot(target *t, uint16_t value, const char **argv)
{
	(void)argv;
	uint32_t high = target_mem_read32(t, SAMD_NVM_USER_ROW_HIGH);
	uint32_t low = target_mem_read32(t, SAMD_NVM_USER_ROW_LOW);

	/* Write address of a word in the row to erase it */
	/* Must be shifted right for 16-bit address, see Datasheet §20.8.8 Address */
	target_mem_write32(t, SAMD_NVMC_ADDRESS, SAMD_NVM_USER_ROW_LOW >> 1);

	/* Issue the erase command */
	target_mem_write32(t, SAMD_NVMC_CTRLA,
	                   SAMD_CTRLA_CMD_KEY | SAMD_CTRLA_CMD_ERASEAUXROW);

	/* Poll for NVM Ready */
	while ((target_mem_read32(t, SAMD_NVMC_INTFLAG) & SAMD_NVMC_READY) == 0)
		if (target_check_error(t))
			return -1;

	/* Modify the low word of the user row */
	low = (low & 0xFFFFFFF8) | ((value << 0 ) & 0x00000007);

	/* Write back */
	target_mem_write32(t, SAMD_NVM_USER_ROW_LOW, low);
	target_mem_write32(t, SAMD_NVM_USER_ROW_HIGH, high);

	/* Issue the page write command */
	target_mem_write32(t, SAMD_NVMC_CTRLA,
	                   SAMD_CTRLA_CMD_KEY | SAMD_CTRLA_CMD_WRITEAUXPAGE);

	return true;
}

static bool samd_cmd_lock_bootprot(target *t, int argc, const char **argv)
{
	/* locks first 0x7 .. 0, 0x6 .. 512, 0x5 .. 1024, ..., 0x0 .. 32768 bytes of flash*/
	if (argc > 2) {
		tc_printf(t, "usage: monitor lock_bootprot [number]\n");
		return false;
	}
	if (argc == 2) {
		uint32_t val = 0;
		if (!parse_unsigned(argv[1], &val)) {
			tc_printf(t, "number must be either decimal or 0x prefixed hexadecimal\n");
			return false;
		}

		if (val > 7) {
			tc_printf(t, "number must be between 0 and 7\n");
			return false;
		}

		return samd_set_bootprot(t, (uint16_t)val, NULL);
	}
	return samd_set_bootprot(t, 0, NULL);
}

static bool samd_cmd_unlock_bootprot(target *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	return samd_set_bootprot(t, 7, NULL);
}

static bool samd_cmd_read_userrow(target *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	tc_printf(t, "User Row: 0x%08x%08x\n",
		target_mem_read32(t, SAMD_NVM_USER_ROW_HIGH),
		target_mem_read32(t, SAMD_NVM_USER_ROW_LOW));

	return true;
}

/*
 * Reads the 128-bit serial number from the NVM
 */
static bool samd_cmd_serial(target *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	tc_printf(t, "Serial Number: 0x");

	for (uint32_t i = 0; i < 4; i++) {
		tc_printf(t, "%08x", target_mem_read32(t, SAMD_NVM_SERIAL(i)));
	}

	tc_printf(t, "\n");

	return true;
}

/*
 * Returns the size (in bytes) of the current SAM D20's flash memory.
 */
static uint32_t samd_flash_size(target *t)
{
	/* Read the Device ID */
	uint32_t did = target_mem_read32(t, SAMD_DSU_DID);

	/* Mask off the device select bits */
	uint8_t devsel = did & SAMD_DID_DEVSEL_MASK;

	/* Shift the maximum flash size (256KB) down as appropriate */
	return (0x40000 >> (devsel % 5));
}

/*
 * Runs the Memory Built In Self Test (MBIST)
 */
static bool samd_cmd_mbist(target *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	/* Write the memory parameters to the DSU */
	target_mem_write32(t, SAMD_DSU_ADDRESS, 0);
	target_mem_write32(t, SAMD_DSU_LENGTH, samd_flash_size(t));

	/* Clear the fail bit */
	target_mem_write32(t, SAMD_DSU_CTRLSTAT, SAMD_STATUSA_FAIL);

	/* Write the MBIST command */
	target_mem_write32(t, SAMD_DSU_CTRLSTAT, SAMD_CTRL_MBIST);

	/* Poll for DSU Ready */
	uint32_t status;
	while (((status = target_mem_read32(t, SAMD_DSU_CTRLSTAT)) &
		(SAMD_STATUSA_DONE | SAMD_STATUSA_PERR | SAMD_STATUSA_FAIL)) == 0)
		if (target_check_error(t))
			return false;

	/* Test the protection error bit in Status A */
	if (status & SAMD_STATUSA_PERR) {
		tc_printf(t, "MBIST not run due to protection error.\n");
		return true;
	}

	/* Test the fail bit in Status A */
	if (status & SAMD_STATUSA_FAIL) {
		tc_printf(t, "MBIST Fail @ 0x%08x\n",
		          target_mem_read32(t, SAMD_DSU_ADDRESS));
	} else {
		tc_printf(t, "MBIST Passed!\n");
	}

	return true;
}
/*
 * Sets the security bit
 */
static bool samd_cmd_ssb(target *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	/* Issue the ssb command */
	target_mem_write32(t, SAMD_NVMC_CTRLA,
	                   SAMD_CTRLA_CMD_KEY | SAMD_CTRLA_CMD_SSB);

	/* Poll for NVM Ready */
	while ((target_mem_read32(t, SAMD_NVMC_INTFLAG) & SAMD_NVMC_READY) == 0)
		if (target_check_error(t))
			return -1;

	tc_printf(t, "Security bit set!\nScan again, attach and issue 'monitor erase_mass' to reset.\n");

	target_reset(t);
	return true;
}
