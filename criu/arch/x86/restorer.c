#include <asm/prctl.h>
#include <unistd.h>

#include "restorer.h"
#include "asm/restorer.h"
#include "asm/fpu.h"
#include "asm/string.h"

#include "syscall.h"
#include "log.h"
#include "cpu.h"

int restore_nonsigframe_gpregs(UserX86RegsEntry *r)
{
#ifdef CONFIG_X86_64
	long ret;
	unsigned long fsgs_base;

	fsgs_base = r->fs_base;
	ret = sys_arch_prctl(ARCH_SET_FS, fsgs_base);
	if (ret) {
		pr_info("SET_FS fail %ld\n", ret);
		return -1;
	}

	fsgs_base = r->gs_base;
	ret = sys_arch_prctl(ARCH_SET_GS, fsgs_base);
	if (ret) {
		pr_info("SET_GS fail %ld\n", ret);
		return -1;
	}
#endif
	return 0;
}

extern unsigned long call32_from_64(void *stack, void *func);

asm (	"	.pushsection .text				\n"
	"	.global restore_set_thread_area			\n"
	"	.code32						\n"
	"restore_set_thread_area:				\n"
	"	movl $"__stringify(__NR32_set_thread_area)",%eax\n"
	"	int $0x80					\n"
	"	ret						\n"
	"	.popsection					\n"
	"	.code64");
extern char restore_set_thread_area;

static void *stack32;

static int prepare_stack32(void)
{

	if (stack32)
		return 0;

	stack32 = (void*)sys_mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_32BIT | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (stack32 == MAP_FAILED) {
		stack32 = NULL;
		pr_err("Failed to allocate stack for 32-bit TLS restore\n");
		return -1;
	}

	return 0;
}

void restore_tls(tls_t *ptls)
{
	int i;

	for (i = 0; i < GDT_ENTRY_TLS_NUM; i++) {
		user_desc_t *desc = &ptls->desc[i];
		int ret;

		if (desc->seg_not_present)
			continue;

		if (prepare_stack32() < 0)
			return;

		builtin_memcpy(stack32, desc, sizeof(user_desc_t));

		/* user_desc parameter for set_thread_area syscall */
		asm volatile ("\t movl %%ebx,%%ebx\n" : :"b"(stack32));
		call32_from_64(stack32 + PAGE_SIZE, &restore_set_thread_area);
		asm volatile ("\t movl %%eax,%0\n" : "=r"(ret));
		if (ret)
			pr_err("Failed to restore TLS descriptor %d in GDT ret %d\n",
					desc->entry_number, ret);
	}

	if (stack32)
		sys_munmap(stack32, PAGE_SIZE);
}
