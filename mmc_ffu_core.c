#include <linux/swap.h>
#include <linux/mmc/mmc_ffu.h>

#include "ffu.h"

/*
 * Map memory into a scatterlist.
 */
unsigned int mmc_ffu_map_sg(struct mmc_ffu_mem *mem, int size,
	struct scatterlist *sglist)
{
	struct scatterlist *sg = sglist;
	unsigned int i;
	unsigned long sz = size;
	unsigned int sctr_len = 0;
	unsigned long len;

	for (i = 0; i < mem->cnt && sz; i++, sz -= len) {
		len = PAGE_SIZE << mem->arr[i].order;

		if (len > sz) {
			len = sz;
			sz = 0;
		}

		sg_set_page(sg, mem->arr[i].page, len, 0);
		sg = sg_next(sg);
		sctr_len++;
	}

	return sctr_len;
}

void mmc_ffu_free_mem(struct mmc_ffu_mem *mem)
{
	if (!mem)
		return;

	while (mem->cnt--)
		__free_pages(mem->arr[mem->cnt].page, mem->arr[mem->cnt].order);

	kfree(mem->arr);
}

/*
 * Cleanup struct mmc_ffu_area.
 */
int mmc_ffu_area_cleanup(struct mmc_ffu_area *area)
{
	sg_free_table(&area->sgtable);
	mmc_ffu_free_mem(&area->mem);
	return 0;
}

/*
 * Allocate a lot of memory, preferably max_sz but at least min_sz. In case
 * there isn't much memory do not exceed 1/16th total low mem pages. Also do
 * not exceed a maximum number of segments and try not to make segments much
 * bigger than maximum segment size.
 */
int mmc_ffu_alloc_mem(struct mmc_ffu_area *area, unsigned long min_sz)
{
	unsigned long max_page_cnt = DIV_ROUND_UP(area->max_tfr, PAGE_SIZE);
	unsigned long min_page_cnt = DIV_ROUND_UP(min_sz, PAGE_SIZE);
	unsigned long max_seg_page_cnt = DIV_ROUND_UP(area->max_seg_sz,
		PAGE_SIZE);
	unsigned long page_cnt = 0;
	unsigned long host_max_segs = area->max_segs;

	/* we divide by 16 to ensure we will not allocate a big amount
	 * of unnecessary pages */
	unsigned long limit = nr_free_buffer_pages() >> 4;

	gfp_t flags = GFP_KERNEL | GFP_DMA | __GFP_NOWARN | __GFP_NORETRY;

	if (max_page_cnt > limit) {
		max_page_cnt = limit;
		area->max_tfr = max_page_cnt * PAGE_SIZE;
	}

	if (min_page_cnt > max_page_cnt)
		min_page_cnt = max_page_cnt;

	if (area->max_segs * max_seg_page_cnt > max_page_cnt)
		area->max_segs = DIV_ROUND_UP(max_page_cnt, max_seg_page_cnt);

	area->mem.arr = kzalloc(sizeof(struct mmc_ffu_pages) * area->max_segs,
		GFP_KERNEL);
	area->mem.cnt = 0;
	if (!area->mem.arr)
		return -ENOMEM;

	while (max_page_cnt) {
		struct page *page;
		unsigned int order;

		if (area->mem.cnt >= area->max_segs) {
			struct mmc_ffu_pages *arr;

			area->max_segs += DIV_ROUND_UP(max_page_cnt,
				max_seg_page_cnt);
			if (area->max_segs > host_max_segs)
				return -ENOMEM;
			arr = krealloc(area->mem.arr,
				sizeof(struct mmc_ffu_pages) * area->max_segs,
				GFP_KERNEL);
			if (!arr)
				return -ENOMEM;

			area->mem.arr = arr;
		}

		order = get_order(max_seg_page_cnt << PAGE_SHIFT);

		do {
			page = alloc_pages(flags, order);
		} while (!page && order--);

		if (!page)
			return -ENOMEM;

		area->mem.arr[area->mem.cnt].page = page;
		area->mem.arr[area->mem.cnt].order = order;
		area->mem.cnt++;
		page_cnt += 1UL << order;
		if (max_page_cnt <= (1UL << order))
			break;
		max_page_cnt -= 1UL << order;
	}

	if (page_cnt < min_page_cnt)
		return -ENOMEM;

	return 0;
}

/*
 * Initialize an area for data transfers.
 * Copy the data to the allocated pages.
 */
int mmc_ffu_area_init(struct mmc_ffu_area *area, struct mmc_card *card,
	const u8 *data)
{
	int ret;
	int i;
	unsigned int length = 0, page_length;

	ret = mmc_ffu_alloc_mem(area, 1);
	if (ret)
		goto out_free;
	for (i = 0; i < area->mem.cnt; i++) {
		if (length > area->max_tfr) {
			ret = -EINVAL;
			goto out_free;
		}
		page_length = PAGE_SIZE << area->mem.arr[i].order;
		memcpy(page_address(area->mem.arr[i].page), data + length,
			min(area->max_tfr - length, page_length));
		length += page_length;
	}

	ret = sg_alloc_table(&area->sgtable, area->mem.cnt, GFP_KERNEL);
	if (ret)
		goto out_free;

	area->sg_len = mmc_ffu_map_sg(&area->mem, area->max_tfr,
		area->sgtable.sgl);

	return 0;

out_free:
	mmc_ffu_free_mem(&area->mem);
	return ret;
}

int mmc_ffu_write(struct mmc_card *card, const u8 *src, u32 arg,
	int size)
{
	int rc;
	struct mmc_ffu_area area = {0};
	int block_size = card->ext_csd.data_sector_size;

	area.max_segs = card->host->max_segs;
	area.max_seg_sz = card->host->max_seg_size & ~(block_size - 1);

	do {
		area.max_tfr = size;
		if (area.max_tfr >> 9 > card->host->max_blk_count)
			area.max_tfr = card->host->max_blk_count << 9;
		if (area.max_tfr > card->host->max_req_size)
			area.max_tfr = card->host->max_req_size;
		if (DIV_ROUND_UP(area.max_tfr, area.max_seg_sz) > area.max_segs)
			area.max_tfr = area.max_segs * area.max_seg_sz;

		rc = mmc_ffu_area_init(&area, card, src);
		if (rc != 0)
			goto exit;

		rc = mmc_simple_transfer(card, area.sgtable.sgl, area.sg_len,
			arg, area.max_tfr / block_size, block_size, 1);
		mmc_ffu_area_cleanup(&area);
		if (rc != 0) {
			pr_err("%s mmc_ffu_simple_transfer %d\n", __func__, rc);
			goto exit;
		}
		src += area.max_tfr;
		size -= area.max_tfr;

	} while (size > 0);

exit:
	return rc;
}

/* Flush all scheduled work from the MMC work queue.
 * and initialize the MMC device */
int mmc_ffu_restart(struct mmc_card *card)
{
	struct mmc_host *host = card->host;
	int err = 0;

	err = mmc_power_save_host(host);
	if (err) {
		pr_warn("%s: going to sleep failed (%d)!!!\n", __func__, err);
		goto exit;
	}

	err = mmc_power_restore_host(host);

exit:

	return err;
}

int mmc_ffu_switch_mode(struct mmc_card *card , int mode)
{
	int err = 0;
	int offset;

	switch(mode) {
	case MMC_FFU_MODE_SET:
	case MMC_FFU_MODE_NORMAL:
		offset = EXT_CSD_MODE_CONFIG;
		break;
	case MMC_FFU_INSTALL_SET:
			offset = EXT_CSD_MODE_OPERATION_CODES;
			mode = 0x1;
			break;
	default:
		err = -EINVAL;
		break;
	}

	if (err == 0) {
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
			offset, mode, card->ext_csd.generic_cmd6_time);
	}

	return err;
}

int mmc_ffu_install(struct mmc_card *card, u8 *ext_csd)
{
	int err;
	u32 timeout;

	/* check mode operation */
	if (!card->ext_csd.ffu_mode_op) {
		/* host switch back to work in normal MMC Read/Write commands */
		err = mmc_ffu_switch_mode(card, MMC_FFU_MODE_NORMAL);
		if (err) {
			pr_err("FFU: %s: switch to normal mode error %d:\n",
				mmc_hostname(card->host), err);
			return err;
		}

		/* restart the eMMC */
		err = mmc_ffu_restart(card);
		if (err) {
			pr_err("FFU: %s: install error %d:\n",
				mmc_hostname(card->host), err);
			return err;
		}
	} else {
		timeout = ext_csd[EXT_CSD_OPERATION_CODE_TIMEOUT];
		if (timeout == 0 || timeout > 0x17) {
			timeout = 0x17;
			pr_warn("FFU: %s: operation code timeout is out "
				"of range. Using maximum timeout.\n",
				mmc_hostname(card->host));
		}

		/* timeout is at millisecond resolution */
		timeout = DIV_ROUND_UP((100 * (1 << timeout)), 1000);

		/* set ext_csd to install mode */
		err = mmc_ffu_switch_mode(card, MMC_FFU_INSTALL_SET);
		if (err) {
			pr_err("FFU: %s: error %d setting install mode\n",
				mmc_hostname(card->host), err);
			return err;
		}
	}

	/* read ext_csd */
	err = mmc_send_ext_csd(card, ext_csd);
	if (err) {
		pr_err("FFU: %s: error %d sending ext_csd\n",
			mmc_hostname(card->host), err);
		return err;
	}

	/* return status */
	err = ext_csd[EXT_CSD_FFU_STATUS];
	if (err) {
		pr_err("FFU: %s: error %d FFU install:\n",
			mmc_hostname(card->host), err);
		return  -EINVAL;
	}

	return 0;
}

