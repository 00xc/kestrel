// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef __KESTREL_H
#define __KESTREL_H

#include <stddef.h>
#include <stdint.h>

void setup_seccomp(void);

struct ks_path {
	const char *path;
	uint32_t len;
};

struct ks_file {
	int fd;
	int cached;
	char *map;
	size_t len;
};

#endif
