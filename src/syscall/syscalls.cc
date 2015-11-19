//#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include "syscalls.h"
#include <errno.h>
#include <stdlib.h>

#include "guest.h"
#include "guestcpustate.h"
#include "Sugar.h"

#define XLATE	guest->getCPUState()->getXlate()

Syscalls::Syscalls(Guest* g)
: guest(g)
, sc_seen_c(0)
, exited(false)
, cpu_state(g->getCPUState())
, mappings(g->getMem())
, binary(g->getBinaryPath())
, log_syscalls(getenv("GUEST_SYSCALLS") ? true : false)
{}

const std::string Syscalls::chroot(getenv("GUEST_CHROOT") ?
	getenv("GUEST_CHROOT") : "");
	
Syscalls::~Syscalls() {}

uint64_t Syscalls::apply(void)
{
	SyscallParams	sp(guest->getSyscallParams());
	return apply(sp);
}

//#include "spimsyscalls.h"
std::unique_ptr<Syscalls> Syscalls::create(Guest* gs)
{
	Syscalls	*sc;

#if 0
	sc = (gs->getArch() == Arch::MIPS32)
		? new SPIMSyscalls(gs)
		: new Syscalls(gs);
#else
	sc = new Syscalls(gs);
#endif
	return std::unique_ptr<Syscalls>(sc);
}

/* pass through */
uint64_t Syscalls::apply(SyscallParams& args)
{
	bool			fakedSyscall;
	int			sys_nr, xlate_sys_nr;
	unsigned long		sc_ret = ~0UL;

	/* translate the syscall number by architecture so that
	   we can have boiler plate implementations centralized
	   in this file */
	sys_nr = args.getSyscall();
	xlate_sys_nr = translateSyscall(sys_nr);

	if (xlate_sys_nr != -1)
		sys_nr = xlate_sys_nr;


	/* check for syscalls which would mess with the thread or process
	   state and otherwise render us useless */
	switch (sys_nr) {
#define BAD_SYSCALL(x)	\
	case x: 	\
		fprintf(stderr, "UNHANDLED SYSCALL: %s\n", #x); \
		assert (0 == 1 && "TRICKY SYSCALL");

	BAD_SYSCALL(SYS_clone)
	BAD_SYSCALL(SYS_fork)
	// BAD_SYSCALL(SYS_exit) ok for now since clone isnt possible
	BAD_SYSCALL(SYS_execve)
	default:
		sc_seen_c++;
		if (sc_seen_c >= MAX_SC_TRACE)
			sc_trace.pop_front();
		sc_trace.push_back(args);
	}

	/* this will hold any memory mapping state across the call.
	   it will be updated by the code which applies the platform
	   dependent syscall to provide a mapping update.  this is done
	   for new mappings, changed mapping, and unmappings */
	GuestMem::Mapping	m;

	fakedSyscall = interceptSyscall(sys_nr, args, sc_ret);
	if (!fakedSyscall) {
		sc_ret = XLATE.apply(*guest, args);
	}

	if (log_syscalls) {
		print(std::cerr, args, (uintptr_t*)&sc_ret);
	}

	return sc_ret;
}

int Syscalls::translateSyscall(int sys_nr) const {
	return XLATE.translateSyscall(sys_nr);
}

std::string Syscalls::getSyscallName(int sys_nr) const {
	return XLATE.getSyscallName(sys_nr);
}

/* DON'T implement any syscall which requires access to
   data in a structure within this function.  this function does not
   understand the potentially different layout of the guests syscalls
   so, syscalls with non-raw pointer data must be handled in the
   architecture specific code */
bool Syscalls::interceptSyscall(
	int sys_nr,
	SyscallParams&		args,
	unsigned long&		sc_ret)
{
	switch (sys_nr) {
	case SYS_exit_group:
		exited = true;
		sc_ret = args.getArg(0);
		return true;
	case SYS_exit:
		exited = true;
		sc_ret = args.getArg(0);
		return true;
	case SYS_close:
		/* do not close stdin, stdout, stderr! */
		if (args.getArg(0) < 3) {
			sc_ret = 0;
			return true;
		}
		return false;
	case SYS_dup2:
		/* do not close stdin, stdout, stderr! */
		if (args.getArg(1) < 3) {
			sc_ret = 0;
			return true;
		}
		return false;
	case SYS_brk:
		if (mappings->sbrk(guest_ptr(args.getArg(0))))
			sc_ret = mappings->brk();
		else
			sc_ret = -ENOMEM;
		return true;
	case SYS_rt_sigaction:
		/* for now totally lie so that code doesn't get
		   executed without us.  we'd rather crash! */
		sc_ret = 0;
		return true;

#ifdef __arm__
#define SYS_mmap	9
#endif

	case SYS_mmap: {
		guest_ptr m;
		sc_ret = mappings->mmap(
			m,
			guest_ptr(args.getArg(0)),
			args.getArg(1),
			args.getArg(2),
			args.getArg(3),
			args.getArg(4),
			args.getArg(5));
		if(sc_ret == 0)
			sc_ret = m;
		return true;
	}
	case SYS_mremap: {
		guest_ptr m;
		sc_ret = mappings->mremap(m,
			guest_ptr(args.getArg(0)),
			args.getArg(1),
			args.getArg(2),
			args.getArg(3),
			guest_ptr(args.getArg(4)));
		if(sc_ret == 0)
			sc_ret = m;
		return true;
	}
	case SYS_mprotect:
		sc_ret = mappings->mprotect(
			guest_ptr(args.getArg(0)),
			args.getArg(1),
			args.getArg(2));
		return true;
	case SYS_munmap:
		sc_ret = mappings->munmap(
			guest_ptr(args.getArg(0)),
			args.getArg(1));
		return true;
	case SYS_readlink:
		std::string path = (char*)args.getArg(0);
		if(path != "/proc/self/exe") break;

		path = binary;
		char* buf = (char*)args.getArg(1);
		ssize_t res, last_res = 0;
		/* repeatedly deref, because exe running does it */
		for(;;) {
			res = readlink(path.c_str(), buf, args.getArg(2));
			if(res < 0) {
				return last_res ? last_res : -errno;
			}
			std::string new_path(buf, res);
			if(path == new_path)
				break;
			path = new_path;
			last_res = res;
		}
		sc_ret = res;
		return true;
	}

	return false;
}

bool SyscallXlate::force_xlate_syscalls;
SyscallXlate::SyscallXlate()
{
	force_xlate_syscalls = getenv("GUEST_XLATE_SYSCALLS")
		? true
		: false;
}

bool SyscallXlate::tryPassthrough(
	Guest& g,
	SyscallParams& args,
	uintptr_t& sc_ret)
{
	auto	m = g.getMem();
	/* if the host and guest are identical, then just pass through */
	if (	!force_xlate_syscalls &&
		m->isFlat() &&
		m->getBase() == NULL &&
		g.getArch() == Arch::getHostArch())
	{
		sc_ret = Syscalls::passthroughSyscall(args);
		return true;
	}

	return false;
}

uintptr_t Syscalls::passthroughSyscall(
	SyscallParams& args)
{
	uintptr_t sc_ret;

	sc_ret = syscall(
		args.getSyscall(),
		args.getArg(0),
		args.getArg(1),
		args.getArg(2),
		args.getArg(3),
		args.getArg(4),
		args.getArg(5));
	/* the low level sycall interface actually returns the error code
	   so we have to extract it from errno if we did blind syscall
	   pass through */
	if(sc_ret >= 0xfffffffffffff001ULL) {
		return -errno;
	}
	return sc_ret;
}

void Syscalls::print(std::ostream& os) const
{
	foreach(it, sc_trace.begin(), sc_trace.end()) {
		SyscallParams	sp = *it;
		print(os, sp, NULL);
	}
}

void Syscalls::print(std::ostream& os, const SyscallParams& sp,
	uintptr_t* result) const
{
	os <<	"Syscall: " << sp.getSyscall() << " : "
		<< getSyscallName(sp.getSyscall());
	os << " {"
		<< (void*)sp.getArg(0) << ", "
		<< (void*)sp.getArg(1) << ", "
		<< (void*)sp.getArg(2) << ", "
		<< (void*)sp.getArg(3) << ", "
		<< (void*)sp.getArg(4) << ", "
		<< (void*)sp.getArg(5) << "}";
	if(result)
		os << " => " << (void*)*result;
	os << std::endl;
}
