// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef __KESTREL_FCACHE_H
#define __KESTREL_FCACHE_H

#include "kestrel.h"
#include "slab.h"

#include <linux/limits.h>
#include <stddef.h>

#define FCACHE_SIZE 256UL

struct ks_cached_file {
	size_t refcnt;
	char *path;
	uint32_t len;
	uint32_t lru;
	struct ks_file *file;
};

struct ks_fcache {
	size_t len;
	uint32_t gen;
	struct ks_slab path_slab;
	struct ks_cached_file items[FCACHE_SIZE];
};

void fcache_init(struct ks_fcache *cache);
struct ks_file *fcache_open(struct ks_fcache *cache,
							struct ks_path *path);
struct ks_file *fcache_insert(struct ks_fcache *cache,
							  struct ks_path *path,
							  struct ks_file *file);
void fcache_close(struct ks_fcache *cache, struct ks_file *file);
struct ks_file *fcache_pop(struct ks_fcache *cache);
#endif
