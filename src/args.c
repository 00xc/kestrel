/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define _GNU_SOURCE
#include "args.h"

#include <err.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct args default_args = {
	.nthreads = 4,
	.root = ".",
	.user = "nobody",
	.port = 8080,
};

static _Noreturn void usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [OPTIONS]\n", progname);
	fprintf(stderr, "\nOptions:\n");
	fprintf(stderr,
			"  -t, --threads NUM   Number of worker threads (default: %lu)\n",
			default_args.nthreads);
	fprintf(stderr,
			"  -r, --root DIR      Server filesystem root (default: %s)\n",
			default_args.root);
	fprintf(stderr,
			"  -u, --user NAME     Username to switch to after chroot (default: %s)\n",
			default_args.user);
	fprintf(stderr,
			"  -p, --port PORT     Port to listen on (default: %u)\n",
			default_args.port);
	fprintf(stderr, "  -h, --help          Show this help message\n");
	exit(EXIT_FAILURE);
}

void parse_args(int argc, char **argv, struct args *args)
{
	const struct option long_options[] = {
		{ "threads", required_argument, 0, 't' },
		{ "root", required_argument, 0, 'r' },
		{ "user", required_argument, 0, 'u' },
		{ "port", required_argument, 0, 'p' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 }
	};
	int opt;

	memcpy(args, &default_args, sizeof(*args));

	while ((opt = getopt_long(argc, argv, "t:r:u:p:h", long_options,
							  NULL)) != -1) {
		switch (opt) {
		case 't':
			args->nthreads = (size_t)atoi(optarg);
			if (!args->nthreads)
				errx(EXIT_FAILURE, "invalid thread count: %s", optarg);
			break;
		case 'r':
			args->root = optarg;
			break;
		case 'u':
			args->user = optarg;
			break;
		case 'p':
			args->port = atoi(optarg);
			if (!args->port)
				err(EXIT_FAILURE, "invalid port: %s", optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
		}
	}
}
