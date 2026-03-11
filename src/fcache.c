/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define _GNU_SOURCE
#include "fcache.h"
#include "kestrel.h"

#include <string.h>

void fcache_init(struct ks_fcache *cache)
{
	cache->len = 0;
	cache->gen = 0;
}

/*
 * Lookup a file, updating it's LRU counter if found.
 */
static struct ks_cached_file *fcache_find(struct ks_fcache *cache,
										  const char *path)
{
	struct ks_cached_file *c;
	size_t i;

	for (i = 0; i < cache->len; ++i) {
		c = &cache->items[i];
		if (!strcmp(c->path, path)) {
			c->lru = ++cache->gen;
			return c;
		}
	}

	return NULL;
}

struct ks_file *fcache_open(struct ks_fcache *cache, const char *path)
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
struct ks_file *fcache_insert(struct ks_fcache *cache, const char *path,
							  struct ks_file *file)
{
	struct ks_cached_file *c = NULL, *f;
	struct ks_file *fevict = NULL;
	size_t i;

	file->cached = 0;
	if (fcache_find(cache, path))
		return NULL;

	if (cache->len >= FCACHE_SIZE) {
		/* Find least recently used file in order to evict it */
		for (i = 0; i < cache->len; ++i) {
			f = &cache->items[i];
			if ((!c || f->lru < c->lru) && !f->refcnt)
				c = f;
		}
		if (!c)
			return NULL;

		fevict = c->file;
		fevict->cached = 0;
	} else {
		c = &cache->items[cache->len++];
	}

	file->cached = 1;
	c->file = file;
	c->lru = ++cache->gen;
	c->refcnt = 1;
	strncpy(c->path, path, PATH_MAX - 1);
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
	return c->file;
}
