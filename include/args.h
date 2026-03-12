// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef __KESTREL_ARGS_H
#define __KESTREL_ARGS_H

#include <stddef.h>
#include <stdint.h>

struct args {
	size_t nthreads;
	const char *root;
	const char *user;
	uint16_t port;
};

void parse_args(int argc, char **argv, struct args *args);

#endif
