#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * reg ranges of every top-level memory node (name "memory" or "memory@...").
 * Fills up to max entries; returns how many ranges exist (may exceed max).
 */
int fdt_memory_ranges(uintptr_t fdt, uint64_t *base, uint64_t *size, int max);
