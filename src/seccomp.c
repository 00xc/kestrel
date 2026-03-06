/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define _GNU_SOURCE
#include <err.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#define ALLOW_SYSCALL(name)                                 \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 1), \
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

void setup_seccomp(void)
{
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
				 (offsetof(struct seccomp_data, arch))),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
				 (offsetof(struct seccomp_data, nr))),

		ALLOW_SYSCALL(close),
		ALLOW_SYSCALL(stat),
		ALLOW_SYSCALL(fstat),
		ALLOW_SYSCALL(lstat),
		ALLOW_SYSCALL(setsockopt),
		ALLOW_SYSCALL(mmap),
		ALLOW_SYSCALL(munmap),
		ALLOW_SYSCALL(mprotect),
		ALLOW_SYSCALL(brk),
		ALLOW_SYSCALL(io_uring_enter),
		ALLOW_SYSCALL(exit),
		ALLOW_SYSCALL(exit_group),
		ALLOW_SYSCALL(madvise),
		ALLOW_SYSCALL(futex),

#ifdef __NR_statx
		ALLOW_SYSCALL(statx),
#endif
#ifdef __NR_newfstatat
		ALLOW_SYSCALL(newfstatat),
#endif

		/* Default deny */
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
	};

	struct sock_fprog prog = {
		.len = sizeof(filter) / sizeof(filter[0]),
		.filter = filter,
	};

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
		err(EXIT_FAILURE, "prctl(PR_SET_NO_NEW_PRIVS)");

	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0)
		err(EXIT_FAILURE, "prctl(PR_SET_SECCOMP)");
}
