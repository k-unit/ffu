/*
 * *  ffu.c
 *
 *  Copyright 2007-2008 Pierre Ossman
 *
 *  Modified by SanDisk Corp., Copyright © 2013 SanDisk Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program includes bug.h, card.h, host.h, mmc.h, scatterlist.h,
 * slab.h, ffu.h & swap.h header files
 * The original, unmodified version of this program – the mmc_test.c
 * file – is obtained under the GPL v2.0 license that is available via
 * http://www.gnu.org/licenses/,
 * or http://www.opensource.org/licenses/gpl-2.0.php
*/

#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/mmc/mmc_ffu.h>
#include <linux/firmware.h>

#include "ffu.h"

int mmc_ffu_invoke(struct mmc_card *card, const char *name)
{
	u8 ext_csd[512];
	int err;
	u32 arg;
	u32 fw_prog_bytes;
	const struct firmware *fw;
	int block_size = card->ext_csd.data_sector_size;

	/* Check if FFU is supported */
	if (!card->ext_csd.ffu_capable) {
		pr_err("FFU: %s: error FFU is not supported %d rev %d\n",
			mmc_hostname(card->host), card->ext_csd.ffu_capable,
			card->ext_csd.rev);
		return -EOPNOTSUPP;
	}

	if (strlen(name) > 512) {
		pr_err("FFU: %s: %.20s is not a valid argument\n",
			mmc_hostname(card->host), name);
		return -EINVAL;
	}

	/* setup FW data buffer */
	err = request_firmware(&fw, name, &card->dev);
	if (err) {
		pr_err("FFU: %s: Firmware request failed %d\n",
			mmc_hostname(card->host), err);
		return err;
	}
	if ((fw->size % block_size)) {
		pr_warn("FFU: %s: Warning %zd firmware data size "
			"is not aligned!!!\n", mmc_hostname(card->host),
			fw->size);
	}

	mmc_claim_host(card->host);

	/* trigger flushing*/
	err = mmc_flush_cache(card);
	if (err) {
		pr_err("FFU: %s: error %d flushing data\n",
			mmc_hostname(card->host), err);
		goto exit;
	}

	/* Read the EXT_CSD */
	err = mmc_send_ext_csd(card, ext_csd);
	if (err) {
		pr_err("FFU: %s: error %d sending ext_csd\n",
			mmc_hostname(card->host), err);
		goto exit;
	}

	/* set CMD ARG */
	arg = ext_csd[EXT_CSD_FFU_ARG] |
		ext_csd[EXT_CSD_FFU_ARG + 1] << 8 |
		ext_csd[EXT_CSD_FFU_ARG + 2] << 16 |
		ext_csd[EXT_CSD_FFU_ARG + 3] << 24;

	/* set device to FFU mode */
	err = mmc_ffu_switch_mode(card, MMC_FFU_MODE_SET);
	if (err) {
		pr_err("FFU: %s: error %d FFU is not supported\n",
			mmc_hostname(card->host), err);
		goto exit;
	}

	err = mmc_ffu_write(card, fw->data, arg, fw->size);
	if (err) {
		pr_err("FFU: %s: write error %d\n",
			mmc_hostname(card->host), err);
		goto exit;
	}
	/* payload  will be checked only in op_mode supported */
	if (card->ext_csd.ffu_mode_op) {
		/* Read the EXT_CSD */
		err = mmc_send_ext_csd(card, ext_csd);
		if (err) {
			pr_err("FFU: %s: error %d sending ext_csd\n",
				mmc_hostname(card->host), err);
			goto exit;
		}

		/* check that the eMMC has received the payload */
		fw_prog_bytes = ext_csd[EXT_CSD_NUM_OF_FW_SEC_PROG] |
			ext_csd[EXT_CSD_NUM_OF_FW_SEC_PROG + 1] << 8 |
			ext_csd[EXT_CSD_NUM_OF_FW_SEC_PROG + 2] << 16 |
			ext_csd[EXT_CSD_NUM_OF_FW_SEC_PROG + 3] << 24;

		/* convert sectors to bytes: multiply by -512B or 4KB as
		   required by the card */
		 fw_prog_bytes *= block_size <<
			 (ext_csd[EXT_CSD_DATA_SECTOR_SIZE] * 3);
		if (fw_prog_bytes != fw->size) {
			err = -EINVAL;
			pr_err("FFU: %s: error %d number of programmed " \
				"fw sector incorrect %d %zd\n", __func__,
				err, fw_prog_bytes, fw->size);
			goto exit;
		}
	}

	err = mmc_ffu_install(card, ext_csd);
	if (err) {
		pr_err("FFU: %s: error firmware install %d\n",
			mmc_hostname(card->host), err);
		goto exit;
	}

exit:
	if (err != 0) {
		/* host switch back to work in normal MMC
		 * Read/Write commands */
		mmc_ffu_switch_mode(card, MMC_FFU_MODE_NORMAL);
	}
	release_firmware(fw);
	mmc_release_host(card->host);
	return err;
}

