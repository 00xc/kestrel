/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define _GNU_SOURCE
#include "fcache.h"
#include "kestrel.h"

#include <err.h>
#include <stdlib.h>
#include <string.h>

void fcache_init(struct ks_fcache *cache)
{
	struct ks_cached_file *f;
	size_t i;

	cache->len = 0;
	cache->gen = 0;
	slab_init(&cache->path_slab, PATH_MAX);

	for (i = 0; i < FCACHE_SIZE; ++i) {
		f = &cache->items[i];
		f->path = slab_alloc(&cache->path_slab);
		if (!f->path)
			err(EXIT_FAILURE, "fcache_init: slab_alloc");
	}
}

/*
 * Lookup a file, updating it's LRU counter if found.
 */
static struct ks_cached_file *fcache_find(struct ks_fcache *cache,
										  struct ks_path *path)
{
	struct ks_cached_file *c;
	size_t i;

	for (i = 0; i < cache->len; ++i) {
		c = &cache->items[i];
		if (c->len == path->len &&
			!memcmp(c->path, path->path, path->len)) {
			c->lru = ++cache->gen;
			return c;
		}
	}

	return NULL;
}

struct ks_file *fcache_open(struct ks_fcache *cache,
							struct ks_path *path)
{
	struct ks_cached_file *c;

	c = fcache_find(cache, path);
	if (!c)
		return NULL;

	c->refcnt++;
	return c->file;
}

/*
 * Insert af file in the cache, if not already present, evicting if
 * necessary an old file. If a file is evicted, it is returned to the
 * caller so that it can adequately be released.
 */
struct ks_file *fcache_insert(struct ks_fcache *cache,
							  struct ks_path *path,
							  struct ks_file *file)
{
	struct ks_cached_file *dst = NULL, *f;
	struct ks_file *fevict = NULL;
	size_t i;

	file->cached = 0;
	if (fcache_find(cache, path))
		return NULL;

	if (cache->len >= FCACHE_SIZE) {
		/* Find least recently used file in order to evict it */
		for (i = 0; i < cache->len; ++i) {
			f = &cache->items[i];
			if ((!dst || f->lru < dst->lru) && !f->refcnt)
				dst = f;
		}
		if (!dst)
			return NULL;

		fevict = dst->file;
		fevict->cached = 0;
	} else {
		dst = &cache->items[cache->len++];
	}

	file->cached = 1;
	dst->file = file;
	dst->lru = ++cache->gen;
	dst->refcnt = 1;
	strncpy(dst->path, path->path, PATH_MAX - 1);
	dst->len = path->len;
	return fevict;
}

void fcache_close(struct ks_fcache *cache, struct ks_file *file)
{
	size_t i;

	for (i = 0; i < cache->len; ++i) {
		if (cache->items[i].file == file) {
			cache->items[i].refcnt--;
			break;
		}
	}
}

struct ks_file *fcache_pop(struct ks_fcache *cache)
{
	struct ks_cached_file *c;

	if (!cache->len)
		return NULL;

	c = &cache->items[--cache->len];
	c->file->cached = 0;
	slab_free(&cache->path_slab, c->path);
	return c->file;
}
