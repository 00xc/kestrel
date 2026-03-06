// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef __KESTREL_SLAB_H
#define __KESTREL_SLAB_H

#include <stddef.h>

#define KESTREL_SLAB_SIZE 4096

#define __malloc __attribute__((__malloc__))

struct ks_slab {
	size_t top;
	size_t itemsz;
	void *stack[KESTREL_SLAB_SIZE];
};

void slab_init(struct ks_slab *slab, size_t size);
void *slab_alloc(struct ks_slab *slab) __malloc;
void slab_free(struct ks_slab *ring, void *p);
void slab_destroy(struct ks_slab *cache);

#endif
