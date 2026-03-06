/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "slab.h"

#include <err.h>
#include <stdlib.h>
#include <string.h>

void slab_init(struct ks_slab *slab, size_t size)
{
	void *p;
	size_t i;

	slab->itemsz = size;

	for (i = 0; i < KESTREL_SLAB_SIZE; ++i) {
		p = malloc(size);
		if (!p)
			err(EXIT_FAILURE, "malloc");
		slab->stack[i] = p;
	}
	slab->top = KESTREL_SLAB_SIZE;
}

__malloc void *slab_alloc(struct ks_slab *slab)
{
	void *ptr;

	if (!slab->top)
		return calloc(1, slab->itemsz);

	ptr = slab->stack[--slab->top];
	return ptr;
}

void slab_free(struct ks_slab *slab, void *p)
{
	if (!p)
		return;

	if (slab->top < KESTREL_SLAB_SIZE)
		slab->stack[slab->top++] = p;
	else
		free(p);
}

void slab_destroy(struct ks_slab *slab)
{
	size_t i;

	for (i = 0; i < slab->top; ++i)
		free(slab->stack[i]);
}
