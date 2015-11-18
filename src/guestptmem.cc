#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include "guestptmem.h"
#include "ptimgarch.h"
#include "ptcpustate.h"
#include "Sugar.h"

GuestPTMem::GuestPTMem(GuestPTImg* gpimg, pid_t in_pid)
: ptimgarch(*gpimg->getPTArch())
, pid(in_pid)
{ /* should I bother with tracking the memory maps?*/
	assert (pid != 0);
}


GuestPTMem::~GuestPTMem(void)
{
	foreach (it, maps.begin(), maps.end()) delete it->second;
	maps.clear();
}

uint8_t GuestPTMem::read8(guest_ptr offset) const
{
	uint64_t	n;
	n = ptrace(PTRACE_PEEKDATA, pid, (void*)(offset.o & ~7UL), NULL);
	return (uint8_t)((n >> (8*(offset.o & 7UL))) & 0xff);
}

#define DEFREAD(x)	\
uint##x##_t GuestPTMem::read##x(guest_ptr offset) const { \
	uint64_t	n;	\
	n = ptrace(PTRACE_PEEKDATA, pid, (void*)offset.o, NULL);	\
	return (uint##x##_t)n;	}
DEFREAD(16) // wrong?
DEFREAD(32) // wrong?
DEFREAD(64)
#undef DEFREAD

#define DEFWRITE(x)	\
void GuestPTMem::write##x(guest_ptr offset, uint##x##_t t)	\
{	uint64_t	n;					\
	long		err;					\
	if (sizeof(t) != 8) {					\
		n = ptrace(PTRACE_PEEKDATA, pid, (void*)offset.o, NULL);\
		n &= ~(uint64_t)(((uint##x##_t)(~0)));			\
		n |= t;							\
	} else								\
		n = t;							\
	err = ptrace(PTRACE_POKEDATA, pid, (void*)offset.o, (void*)n);	\
	assert (err != -1 && "BAD WRITE"); }
DEFWRITE(8)
DEFWRITE(16)
DEFWRITE(32)
DEFWRITE(64)
#undef DEFWRITE


void GuestPTMem::memcpy(guest_ptr dest, const void* src, size_t len)
{
	unsigned	rem;

	rem = len % sizeof(long);
	if (rem > 0) {
		for (unsigned i = 0; i < rem; i++)
			write8(dest + i, ((const char*)src)[i]);
	}

	len -= rem;
	if (len == 0) return;

	ptimgarch.getPTCPU().copyIn(dest + rem, (const char*)src + rem, len);
}

void GuestPTMem::memcpy(void* dest, guest_ptr src, size_t len) const
{
	uint8_t		*v8 = (uint8_t*)dest;

	/* fast path */
	if ((src.o & 7UL) == 0 && (len & 7UL) == 0) {
		uint64_t	*v64 = (uint64_t*)dest;
		for (unsigned i = 0; i < len / 8; i++)
			v64[i] = read64(src + i*8);
		return;
	}

	/* XXX: this can be done faster, but have to worry about alignments. */
	for (unsigned i = 0; i < len; i++)
		v8[i] = read8(src + i);
}

void GuestPTMem::memset(guest_ptr dest, char d, size_t len)
{
	for (unsigned i = 0; i < len; i++)
		write8(dest + i, d);
}

int GuestPTMem::strlen(guest_ptr p) const
{
	int	n = 0;
	while (read8(p)) {
		p.o++;
		n++;
	}
	return n;
}

bool GuestPTMem::sbrk(guest_ptr new_top) { assert (0 == 1 && "STUB"); }

int GuestPTMem::mmap(
	guest_ptr& result, guest_ptr addr, size_t length,
	int prot, int flags, int fd, off_t offset)
{
#ifdef __arm__
#define SYS_mmap 9
#endif
	SyscallParams	sp(SYS_mmap, addr.o, length, prot, flags, fd, offset);
	result.o = ptimgarch.dispatchSysCall(sp);
	return ((void*)result.o == MAP_FAILED) ? -1 : 0;
}

int GuestPTMem::mprotect(guest_ptr offset, size_t length, int prot)
{
	SyscallParams	sp(SYS_mprotect, offset.o, length, prot, 0, 0, 0);
	ptimgarch.dispatchSysCall(sp);
	return 0;
}

int GuestPTMem::munmap(guest_ptr offset, size_t length)
{
	SyscallParams	sp(SYS_munmap, offset.o, length, 0, 0, 0, 0);
	ptimgarch.dispatchSysCall(sp);
	return 0;
}

int GuestPTMem::mremap(
	guest_ptr& result, guest_ptr old_offset,
	size_t old_length, size_t new_length,
	int flags, guest_ptr new_offset)
{
	assert (0 == 1 && "STUB");
	return 0;
}


void GuestPTMem::import(GuestMem* m)
{
	base = m->base;
	top_brick = m->top_brick;
	base_brick = m->base_brick;
	reserve_brick = m->reserve_brick;
	force_flat = m->force_flat;
	// syspage_data = m->syspage_data;

	foreach (it, m->maps.begin(), m->maps.end()) {
		guest_ptr	p(it->first);
		Mapping		*m(it->second);

		recordMapping(*m);
		if (m->name != NULL)
			nameMapping(p, *m->name);
	}
}
