#ifndef _FFU_H_
#define _FFU_H_

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
/**
 * struct mmc_ffu_pages - pages allocated by 'alloc_pages()'.
 * @page: first page in the allocation
 * @order: order of the number of pages allocated
 */
struct mmc_ffu_pages {
	struct page *page;
	unsigned int order;
};

/**
 * struct mmc_ffu_mem - allocated memory.
 * @arr: array of allocations
 * @cnt: number of allocations
 */
struct mmc_ffu_mem {
	struct mmc_ffu_pages *arr;
	unsigned int cnt;
};

struct mmc_ffu_area {
	unsigned long max_sz;
	unsigned int max_tfr;
	unsigned int max_segs;
	unsigned int max_seg_sz;
	unsigned int blocks;
	unsigned int sg_len;
	struct mmc_ffu_mem mem;
	struct sg_table sgtable;
};

unsigned int mmc_ffu_map_sg(struct mmc_ffu_mem *mem, int size,
	struct scatterlist *sglist);
void mmc_ffu_free_mem(struct mmc_ffu_mem *mem);
int mmc_ffu_area_cleanup(struct mmc_ffu_area *area);
int mmc_ffu_alloc_mem(struct mmc_ffu_area *area, unsigned long min_sz);
int mmc_ffu_area_init(struct mmc_ffu_area *area, struct mmc_card *card,
	const u8 *data);
int mmc_ffu_write(struct mmc_card *card, const u8 *src, u32 arg,
	int size);
int mmc_ffu_restart(struct mmc_card *card);
int mmc_ffu_switch_mode(struct mmc_card *card , int mode);
int mmc_ffu_install(struct mmc_card *card, u8 *ext_csd);

#endif

