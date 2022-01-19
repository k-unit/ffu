#include <linux/mmc/mmc_ffu.h>
#include <linux/swap.h>
#include "ffu.h"

#include <linux/mmc/kut_host.h>
#include <linux/mmc/kut_core.h>
#include <linux/mmc/kut_bus.h>
#include <linux/kut_mmzone.h>
#include <linux/kut_random.h>

#include <unit_test.h>

#include <string.h>
 
#ifdef CONFIG_KUT_COLOURS
#undef C_ITALIC
#define C_ITALIC "\033[03m"
#undef C_NORMAL
#define C_NORMAL "\033[00;00;00m"
#else
#undef C_ITALIC
#define C_ITALIC ""
#undef C_NORMAL
#define C_NORMAL ""
#endif

#define VER_DEFAULT 7
#define VER_NONE -1

/* nr_free_buffer_pages() >> 4) in multiples of block size */
#define FFU_SINGLE_XFER_LIMIT_512B 50000
#define FFU_SINGLE_XFER_LIMIT_4KB 6250

#define MODE_OPERATION_CODE_FFU_INSTALL 0x01
#define MODE_OPERATION_CODE_FFU_ABORT 0x02
#define MODE_OPERATION_CODE_FFU_NONE -1

#define MODE_CONFIG_NORMAL 0x00
#define MODE_CONFIG_FFU 0x01
#define MODE_CONFIG_VENDOR_SPECIFIC 0x10
#define MODE_CONFIG_NONE -1

#define DATA_SECTOR_SIZE_512B 0x00
#define DATA_SECTOR_SIZE_4KB 0x01
#define DATA_SECTOR_SIZE_NONE -1

#define FW_CONFIG_UPDATE_DISABLE_OFF 0x00
#define FW_CONFIG_UPDATE_DISABLE_ON 0x01
#define FW_CONFIG_UPDATE_DISABLE_NONE -1

#define FFU_ARG_NEW 0x10
#define FFU_ARG_NONE -1

#define FFU_STATUS_SUCCESS 0x0
#define FFU_STATUS_GENERAL_ERROR 0x10
#define FFU_STATUS_FIRMWARE_INSTALL_ERROR 0x11
#define FFU_STATUS_ERROR_DOWNLOAD_FIRMWARE 0x12
#define FFU_STATUS_NONE -1

#define FFU_FEATURES_NO_SUPPORT_MOC 0x00
#define FFU_FEATURES_SUPPORT_MOC 0x01
#define FFU_FEATURES_NONE -1

#define FFU_MODE_OPERATION_CODES_DEFAULT 0x00
#define FFU_MODE_OPERATION_CODES_MAX 0x17

#define SUPPORTED_MODES_NO_VSM_NO_FFU 0x00
#define SUPPORTED_MODES_NO_VSM_FFU 0x01
#define SUPPORTED_MODES_VSM_NO_FFU 0x10
#define SUPPORTED_MODES_VSM_FFU 0x11
#define SUPPORTED_MODES_NONE -1

struct s8_with_name {
	s8 opt;
	char *name;
};
#define MAKE_S8_WITH_NAME(_s8_opt_) { _s8_opt_, #_s8_opt_ }

static int verify_mmc_ffu_mem(struct sg_table *table, struct mmc_ffu_mem *mem,
	unsigned long length)
{
	struct scatterlist *sgl;
	int i;

	for_each_sg(table->sgl, sgl, table->orig_nents, i) {
		struct page *page;
		unsigned long cur_data_size;

		page = sg_page(sgl);
		if (!page) {
			pr_err("failed to get page of sg entry %d\n", i);
			return -1;
		}

		if (page != mem->arr[i].page) {
			pr_err("sg page != mem->arr page (sg entry: %d)\n", i);
			return -2;
		}

		/* the data in the final entry may not consume the full length
		 * of the allocated pages */
		cur_data_size = PAGE_SIZE << mem->arr[i].order;
		if (i == table->orig_nents - 1) {
			unsigned long cur_alloc_size = cur_data_size;

			cur_data_size = min(cur_alloc_size, length);
		}
		length -= cur_data_size;

		if (sgl->length != cur_data_size) {
			pr_err("sg length != PAGE_SIZE << mem->arr.order " \
				"(sg entry: %d)\n", i);
			return -3;
		}

		if (sgl->offset) {
			pr_err("sg offset = %u (sg entry: %d)\n", sgl->offset,
				i);
			return -4;
		}
	}
	if (i != mem->cnt) {
		pr_err("number of nents: expected %d, got %d\n", mem->cnt, i);
		return -5;
	}

	return 0;
}

static int ffu_map_sg_test(int nents)
{
	struct sg_table table = {0};
	struct mmc_ffu_mem mem = {0};
	int ret, i, size = 0;

	/* allocate sg table */
	ret = sg_alloc_table(&table, nents, GFP_KERNEL);
	if (ret) {
		pr_err("sg_alloc_table failed\n");
		goto exit;
	}

	/* initialize mem */
	mem.arr = kmalloc(nents * sizeof(struct mmc_ffu_pages), GFP_KERNEL);
	if (!mem.arr) {
		pr_err("allocating mem.arr failed\n");
		ret = -ENOMEM;
		goto exit;
	}
	for (i = 0; i < nents; i++) {
		mem.arr[i].order = random() % kut_mem_pressure_get();
		mem.arr[i].page = alloc_pages(GFP_KERNEL, mem.arr[i].order);

		if (!mem.arr[i].page) {
			pr_err("allocating 1<<%d pages failed\n",
				mem.arr[i].order);
			ret = -ENOMEM;
			goto exit;
		}

		size += PAGE_SIZE << mem.arr[i].order;
	}
	mem.cnt = nents;

	/* map mem pages to sg table */
	ret = mmc_ffu_map_sg(&mem, size, table.sgl);
	if (ret != nents) {
		pr_err("mmc_ffu_map_sg(): expected %d got %d\n.", nents, ret);
		ret = -1;
		goto exit;
	}

	/* verify sg table */
	ret = verify_mmc_ffu_mem(&table, &mem, size);

exit:
	/* cleanup */
	if (mem.arr) {
		for (i = 0; i < mem.cnt; i++)
			__free_pages(mem.arr[i].page, mem.arr[i].order);
		kfree(mem.arr);
	}

	sg_free_table(&table);

	return ret;
}

static int ffu_map_sg_no_chain(void)
{
	return ffu_map_sg_test(SG_MAX_SINGLE_ALLOC);
}

static int ffu_map_sg_multi_chain(void)
{
	int nents = 3 * SG_MAX_SINGLE_ALLOC + (SG_MAX_SINGLE_ALLOC >> 1);

	return ffu_map_sg_test(nents);
}

static int __ffu_area_init_test(int blks, s8 data_sector_size)
{
	struct mmc_host *host;
	struct mmc_card *card;
	struct mmc_ffu_area area = {0};
	int block_size, size, ret;
	unsigned long limit;
	u8 *src = NULL, *src_verify = NULL;

	kut_mmc_ext_csd_set_ffu(VER_DEFAULT, FFU_STATUS_SUCCESS,
		MODE_OPERATION_CODE_FFU_INSTALL, MODE_CONFIG_FFU,
		data_sector_size, FW_CONFIG_UPDATE_DISABLE_OFF, FFU_ARG_NEW,
		FFU_FEATURES_NO_SUPPORT_MOC, FFU_MODE_OPERATION_CODES_DEFAULT,
		SUPPORTED_MODES_NO_VSM_FFU);
	kut_mmc_init(NULL, NULL, &host, &card, 0);

	/* use BLOCKS_TO_BYTES() after setting block size in ext_csd */
	size = BLOCKS_TO_BYTES(blks);
	src = kzalloc(size, GFP_KERNEL);
	src_verify = kzalloc(size, GFP_KERNEL);
	if (!src || !src_verify) {
		ret = -ENOMEM;
		goto exit;
	}
	kut_random_buf((char*)src, (unsigned long)size);

	/* tested size should be <= card->host->max_blk_count and must be
	 * block_size aligned */
	block_size = card->ext_csd.data_sector_size;
	area.max_segs = card->host->max_segs;
	area.max_seg_sz = card->host->max_seg_size & ~(block_size - 1);
	area.max_tfr = size;

	ret = mmc_ffu_area_init(&area, card, (const u8*)src);
	if (ret)
		goto exit;

	/* validate area sg table: allow for mmc_ffu_area_init() limiting the
	 * transfer size depending to the number of free pages available in the
	 * system. */
	limit = (nr_free_buffer_pages() >> 4) * PAGE_SIZE;
	size = min((unsigned long)size, limit);
	ret = verify_mmc_ffu_mem(&area.sgtable, &area.mem, size);

	/* verify area.sg_len */
	if (area.sg_len != area.sgtable.orig_nents)
		ret = -1;

	/* verify data correctness */
	sg_copy_to_buffer(area.sgtable.sgl, area.sg_len, src_verify, size);
	if (memcmp(src, src_verify, size))
		ret = -1;

	mmc_ffu_area_cleanup(&area);
exit:
	kfree(src);
	kfree(src_verify);
	kut_mmc_uninit(NULL, host, card);
	return ret;
}

static int ffu_area_init_test(void)
{
	int i, j, ret = 0;
	s8 block_sizes[2] = {
		DATA_SECTOR_SIZE_512B,
		DATA_SECTOR_SIZE_4KB,
	};
	/* [size in blocks] [memory pressure] [expected result] */
	int tests[][3] = {
		/* minimal size aligned to card->ext_csd.data_sector_size */
		{ 1, KUT_MEM_SCARCE, 0 },
		/* size is not page aligned */
		{ 512, KUT_MEM_SCARCE, 0 },
		{ 1024, KUT_MEM_SCARCE, 0 },
		{ FFU_SINGLE_XFER_LIMIT_512B - 1, KUT_MEM_AVERAGE, 0 },
		{ FFU_SINGLE_XFER_LIMIT_4KB - 1, KUT_MEM_AVERAGE, 0 },
		/* limited size: (nr_free_buffer_pages() >> 4) */
		{ FFU_SINGLE_XFER_LIMIT_512B, KUT_MEM_AVERAGE, 0 },
		{ FFU_SINGLE_XFER_LIMIT_4KB, KUT_MEM_AVERAGE, 0 },
		/* cause sg allocation to exceed card->host->max_segs */
		{ 65536, KUT_MEM_SCARCE, -ENOMEM },
		/* maximum request size: card->host->max_req_size */
		{ 65536, KUT_MEM_AVERAGE, 0 },
	};

	for (i = 0; i < ARRAY_SZ(block_sizes); i++) {
		for (j = 0; j < ARRAY_SZ(tests); j++) {
			kut_mem_pressure_set(tests[j][1]);

			if (__ffu_area_init_test(tests[j][0], block_sizes[i]) !=
				tests[j][2]) {
				pr_err("failed with size: %d (max order: %d)\n",
					tests[j][0], tests[j][1]);
				ret = -1;
			}
		}
	}

	return ret;
}

static int __mmc_ffu_write_test(s8 data_sector_size, int size)
{
	struct mmc_host *host = NULL;
	struct mmc_card *card = NULL;
	struct list_head xfer_expected = {0};
	struct kut_mmc_card_xfer *tmp, *xfer = NULL;
	int ret;
	u8 *src = NULL, ext_csd[512];
	u32 arg;
	int remaining, max_xfer_length;

	/* setup extended csd */
	kut_mmc_ext_csd_set_ffu(VER_DEFAULT, FFU_STATUS_SUCCESS,
		MODE_OPERATION_CODE_FFU_INSTALL, MODE_CONFIG_FFU,
		data_sector_size, FW_CONFIG_UPDATE_DISABLE_OFF, FFU_ARG_NEW,
		FFU_FEATURES_NO_SUPPORT_MOC, FFU_MODE_OPERATION_CODES_DEFAULT,
		SUPPORTED_MODES_NO_VSM_FFU);
	kut_mmc_init(NULL, NULL, &host, &card, 0);

	ret = mmc_send_ext_csd(card, ext_csd);
	if (ret)
		goto exit;

	arg = ext_csd[EXT_CSD_FFU_ARG] |
		ext_csd[EXT_CSD_FFU_ARG + 1] << 8 |
		ext_csd[EXT_CSD_FFU_ARG + 2] << 16 |
		ext_csd[EXT_CSD_FFU_ARG + 3] << 24;

	/* setup src buffer */
	src = kzalloc(size, GFP_KERNEL);
	if (!src)
		return -ENOMEM;
	kut_random_buf((char*)src, (unsigned long)size);

	/* setup transfer verification segments */
	INIT_LIST_HEAD(&xfer_expected);
	max_xfer_length = PAGE_SIZE * (nr_free_buffer_pages() >> 4);
	remaining = size;

	while (remaining) {
		int written = size - remaining;
		
		xfer = kzalloc(sizeof(struct kut_mmc_card_xfer), GFP_KERNEL);
		if (!xfer) {
			ret = -ENOMEM;
			goto exit;
		}

		xfer->length = min(max_xfer_length, remaining);
		xfer->buf = calloc(xfer->length, sizeof(char));
		if (!xfer->buf)
			goto exit;
		memcpy(xfer->buf, src + written, xfer->length);

		INIT_LIST_HEAD(&xfer->list);

		xfer->write = true;
		xfer->addr = arg; /* in ffu mode, the semantics of arg is vendor
				     specific and is not necessarily 'address'.
				     as such, it does not get incremented in
				     subsequent write iterations */
		list_add_tail(&xfer->list, &xfer_expected);
		remaining -= xfer->length;
	}

	/* issue ffu write */
	ret = mmc_ffu_write(card, (const u8*)src, arg, size);
	if (ret)
		goto exit;

	/* verify transfer */
	ret = kut_mmc_xfer_check(card, &xfer_expected);

exit:
	/* clean up */
	list_for_each_entry_safe(xfer, tmp, &xfer_expected, list) {
		list_del(&xfer->list);
		kfree(xfer->buf);
		kfree(xfer);
	}

	kut_mmc_uninit(NULL, host, card);
	kfree(src);

	return ret;
}

static int mmc_ffu_write_test(void)
{
	int i, j, ret;
	s8 block_sizes[] = {
		DATA_SECTOR_SIZE_512B,
		DATA_SECTOR_SIZE_4KB,
	};
	int sizes[] = {
		512, /* should fail with block size: DATA_SECTOR_SIZE_4KB */
		4096,
		5000, /* should always fail */
		PAGE_SIZE * (nr_free_buffer_pages() >> 4), /* max mem allowed */
		PAGE_SIZE * (nr_free_buffer_pages() >> 4) + 4096,
	};

	for (i = 0; i < ARRAY_SZ(block_sizes); i++) {
		for (j = 0; j < ARRAY_SZ(sizes); j++) {
			int block_size = i ? 4096 : 512;

			ret = __mmc_ffu_write_test(block_sizes[i], sizes[j]);

			/* ffu write should fail if and only if firmware size is
			 * not aligned to the device's block size */
			if (!!ret ^ !!(sizes[j] % block_size)) {
				pr_err("failed on block size: %dB, " \
					"xfer size: %dB (err:%d)\n", block_size,
					sizes[j], ret);
				goto exit;
			}
		}

		ret = 0;
	}

exit:
	return ret;
}

static int mmc_ffu_install_test_one(struct s8_with_name *block_size,
		struct s8_with_name *ffu_feature, u8 timeout,
		struct s8_with_name *ffu_status)
{
	struct mmc_host *host = NULL;
	struct mmc_card *card = NULL;
	u8 ext_csd[512];
	int st;
	int ret = 0;

	printf("%sTesting:%s %s, %s, timeout: 0x%x, %s (%d)\n", C_ITALIC,
		C_NORMAL, block_size->name, ffu_feature->name, timeout,
		ffu_status->name, ffu_status->opt);

	kut_mmc_ext_csd_set_ffu(VER_DEFAULT, ffu_status->opt,
		MODE_OPERATION_CODE_FFU_INSTALL, MODE_CONFIG_FFU,
		block_size->opt, FW_CONFIG_UPDATE_DISABLE_OFF, FFU_ARG_NEW,
		ffu_feature->opt, timeout, SUPPORTED_MODES_NO_VSM_FFU);

	kut_mmc_init(NULL, NULL, &host, &card, 0);
	ret = mmc_send_ext_csd(card, ext_csd);
	if (ret)
		goto exit;

	st = mmc_ffu_install(card, ext_csd);
	if (!!st != !!ffu_status->opt)
		ret = -1;

	/* JEDEC Standard No. 84-XXX, Annex A Application Notes
	 *
	 * A.12 Field Firmware Update
	 * The following flow describes the positive oriented FFU flow.
	 *
	 *                +-------------------------------------+
	 *                | Using CMD8 host checks if FFU is    |
	 *                | supported by the device by reading  |
	 *                | SUPPORTED_MODES field               |
	 *                +-------------------------------------+
	 *                                   |
	 *                                   V
	 *                +-------------------------------------+
	 *                | Host checks that Update_Disable     |
	 *                | bit in FW_CONFIG [169] is '0'       |
	 *                +-------------------------------------+
	 *                                   |
	 *                                   V
	 *                +-------------------------------------+
	 *                | Host sets MODE_CONFIG to FFU Mode   |
	 *                +-------------------------------------+
	 *                                   |
	 *                                   V
	 *                +-------------------------------------+
	 *                | Host sets MODE_CONFIG to FFU Mode   |
	 *                +-------------------------------------+
	 *                             |           |
	 *      MODE_OPERATION_CODES   |           |   MODE_OPERATION_CODES
	 *           supported         |           |      not supported
	 *                             V           V
	 * +-------------------------------+   +-------------------------------*
	 * | Host sets                     |   |                               |
	 * | MODE_OPERATION_CODES to       |   | Host sets MODE_CONFIG to      |
	 * | FFU_INSTALL which             |   | NORMAL and performs           |
	 * | automatically sets            |   | CMD0/HW Reset/Power cycle     |
	 * | MODE_CONFIG to NORMAL         |   |                               |
	 * +-------------------------------+   +-------------------------------*
	 *
	 * */
	if (((ext_csd[EXT_CSD_FFU_FEATURES] != FFU_FEATURES_NO_SUPPORT_MOC) ||
			(ext_csd[EXT_CSD_MODE_CONFIG] != MMC_FFU_MODE_NORMAL))
			&&
		((ext_csd[EXT_CSD_FFU_FEATURES] != FFU_FEATURES_SUPPORT_MOC) ||
			(ext_csd[EXT_CSD_MODE_OPERATION_CODES] !=
			 MODE_OPERATION_CODE_FFU_INSTALL))) {
		ret = -1;
	}

exit:
	kut_mmc_uninit(NULL, host, card);
	return ret;
}

static int mmc_ffu_install_test(void)
{
	int ret = 0;
	struct s8_with_name block_sizes[2] = {
		MAKE_S8_WITH_NAME(DATA_SECTOR_SIZE_512B),
		MAKE_S8_WITH_NAME(DATA_SECTOR_SIZE_4KB),
	};
	int size;

	for (size = 0; size < ARRAY_SZ(block_sizes); size++) {
		struct s8_with_name ffu_features[2] = {
			MAKE_S8_WITH_NAME(FFU_FEATURES_NO_SUPPORT_MOC),
			MAKE_S8_WITH_NAME(FFU_FEATURES_SUPPORT_MOC),
		};
		int mode;

		for (mode = 0; mode < ARRAY_SZ(ffu_features); mode++) {
			u8 timeout;

			for (timeout = FFU_MODE_OPERATION_CODES_DEFAULT;
					timeout <= FFU_MODE_OPERATION_CODES_MAX;
					timeout++) {
				struct s8_with_name ffu_status[4] = {
					MAKE_S8_WITH_NAME(FFU_STATUS_SUCCESS),
					MAKE_S8_WITH_NAME(FFU_STATUS_GENERAL_ERROR),
					MAKE_S8_WITH_NAME(
						FFU_STATUS_FIRMWARE_INSTALL_ERROR),
					MAKE_S8_WITH_NAME(
						FFU_STATUS_ERROR_DOWNLOAD_FIRMWARE),
				};
				int status;

				if ((ffu_features[mode].opt ==
						FFU_FEATURES_SUPPORT_MOC) ^ !!timeout) {
					/* FFU_FEATURES_SUPPORT_MOC requires
					       0x1 <= timeout <= 0x17 requires
					   FFU_FEATURES_NO_SUPPORT_MOC requires
					       timeout == 0x00 */
					continue;
				}

				for (status = 0; status < ARRAY_SZ(ffu_status);
						status++) {
					printf("\n");
					if (mmc_ffu_install_test_one(&block_sizes[size],
							&ffu_features[mode],
							timeout,
							&ffu_status[status])) {
						ret = -1;
					}
				}
			}
		}
	}

	return ret;
}

static struct single_test ffu_tests[] = {
	{
		description: "ffu map sg - no chaining",
		func: ffu_map_sg_no_chain,
	},
	{
		description: "ffu map sg - multi chaining",
		func: ffu_map_sg_multi_chain,
	},
	{
		description: "area init test",
		func: ffu_area_init_test,
	},
	{
		description: "ffu write test",
		func: mmc_ffu_write_test,
	},
	{
		description: "ffu install test",
		func: mmc_ffu_install_test,
	},
};

static int ffu_pre_test(void)
{
	kut_mmc_ext_csd_reset(7);
	return 0;
}

struct unit_test ut_ffu = {
	.module = "ffu",
	.description = "Field Firmware Upgrade",
	.pre_single_test = ffu_pre_test,
	.tests = ffu_tests,
	.count = ARRAY_SZ(ffu_tests),
};

