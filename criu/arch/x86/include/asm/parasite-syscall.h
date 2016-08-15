#ifndef __CR_ASM_PARASITE_SYSCALL_H__
#define __CR_ASM_PARASITE_SYSCALL_H__

#include "asm/types.h"

struct parasite_ctl;

#define ARCH_SI_TRAP SI_KERNEL

#define __NR(syscall, compat)	((compat) ? __NR32_##syscall : __NR_##syscall)

/*
 * For x86_32 __NR_mmap inside the kernel represents old_mmap system
 * call, but since we didn't use it yet lets go further and simply
 * define own alias for __NR_mmap2 which would allow us to unify code
 * between 32 and 64 bits version.
 */
#define __NR32_mmap __NR32_mmap2


void parasite_setup_regs(unsigned long new_ip, void *stack, user_regs_struct_t *regs);

void *mmap_seized(struct parasite_ctl *ctl,
		  void *addr, size_t length, int prot,
		  int flags, int fd, off_t offset);

#endif
