#include "guest.h"
#include "guestcpustate.h"
#include "guestabi.h"
#include "abi/linux_abi.h"

bool GuestABI::use_linux_sysenter = true;

RegStrABI::RegStrABI(
	Guest& _g,
	const char** sc_regs,
	const char* sc_ret,
	const char* exit_reg,
	bool	_is_32bit)
: GuestABI(_g)
, is_32bit(_is_32bit)
{
	GuestCPUState	*cpu(g.getCPUState());
	unsigned	i = 0;

	if (sc_regs) {
		for (; sc_regs[i] && i < 7; i++) {
			sc_reg_off[i] = cpu->name2Off(sc_regs[i]);
		}
	}
	sc_reg_off[i] = ~0U;

	scret_reg_off = cpu->name2Off(sc_ret);
	exit_reg_off = cpu->name2Off(exit_reg);
}

SyscallParams RegStrABI::getSyscallParams(void) const
{
	const uint8_t	*dat;
	uint64_t	v[7];

	memset(v, 0, sizeof(v));

	dat = (const uint8_t*)g.getCPUState()->getStateData();
	for (unsigned i = 0; sc_reg_off[i] != ~0U && i < 7; i++) {
		v[i] = *((const uint64_t*)(dat + sc_reg_off[i]));
		if (is_32bit) v[i] &= 0xffffffff;
	}

	return SyscallParams(v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
}

uint64_t RegStrABI::getSyscallResult(void) const
{
	const void	*dat;

	dat = (const void*)
		((const uint8_t*)g.getCPUState()->getStateData() + scret_reg_off);

	return (is_32bit)
		? *((const uint32_t*)dat)
		: *((const uint64_t*)dat);
}

void RegStrABI::setSyscallResult(uint64_t ret)
{
	void	*dat;

	dat = (void*)((uint8_t*)g.getCPUState()->getStateData() + scret_reg_off);
	if (is_32bit) {
		*((uint32_t*)dat) = ret & 0xffffffff;
	} else {
		*((uint64_t*)dat) = ret;
	}
}

uint64_t RegStrABI::getExitCode(void) const
{
	const uint8_t	*dat((const uint8_t*)g.getCPUState()->getStateData());
	uint64_t	v;

	v = *((const uint64_t*)(dat + exit_reg_off));
	if (is_32bit) v &= 0xffffffff;

	return v;
}

GuestABI* GuestABI::create(Guest& g)
{
#if 0
	if (g.getArch() == Arch::MIPS32) 
		return new RegStrABI(g, ABI_SPIM_MIPS32);
#endif
	/* XXX: windows support? */
	return LinuxABI::create(g);
}