#pragma once
#include <stdint.h>

#include "board.h"

#define FB_PHYS	FB_BASE

struct mmu_info {
	uint64_t img_lo, img_hi, ttbr;
	uint64_t sctlr_before, sctlr_after;
	int n_l3;
};

int mmu_enable(struct mmu_info *info);
uint64_t mmu_probe(uintptr_t va);
/* After mmu_enable: map [pa, pa + size) as Normal write-back in 32 MB blocks (free RAM only:
 * no protected carve-out may share a block, speculative reads would fault). 0 on success. */
int mmu_map_wb(uint64_t pa, uint64_t size);
/* Clean+invalidate [a, a + an) and [b, b + bn) to memory, then MMU and caches off again. */
void mmu_disable_clean(uint64_t a, uint64_t an, uint64_t b, uint64_t bn);
/* MMU off: clean+invalidate by physical address (lines ABL may have left). */
void dcache_civac_range(uint64_t start, uint64_t end);
