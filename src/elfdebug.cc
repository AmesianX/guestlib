#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include "arch.h"
#include "symbols.h"
#include "guestmem.h"
#include "elfimg.h"
#include "elfdebug.h"

#include <stdio.h>

Symbols* ElfDebug::getSymsAll(ElfDebug& ed, uintptr_t base)
{
	auto ret = new Symbols();
	while (auto s = ed.nextSym()) {
		symaddr_t	addr = s->getBaseAddr();
		if (s->isCode() && s->getName().size() > 0 && addr) {
			if (!ed.isExec())
				addr += base;
			ret->addSym(s->getName(), addr, s->getLength());
		}
	}
	return ret;
}

Symbols* ElfDebug::getSyms(const char* elf_path, uintptr_t base)
{
	ElfDebug	ed(elf_path);
	return ed.is_valid ? getSymsAll(ed, base) : nullptr;
}

Symbols* ElfDebug::getSyms(const void* base)
{
	ElfDebug ed(base);
	return ed.is_valid ? getSymsAll(ed, (uintptr_t)base) : nullptr;
}

Symbols* ElfDebug::getLinkageSyms(
	const GuestMem* m, const char* elf_path)
{
	Symbols		*ret;
	ElfDebug	ed(elf_path);
	if (ed.is_valid == false) {
		return NULL;
	}

	ret = new Symbols();
	while (auto s = ed.nextLinkageSym(m)) {
		ret->addSym(s->getName(), s->getBaseAddr(), s->getLength());
	}
	return ret;

}

ElfDebug::ElfDebug(const void* base)
: is_valid(false)
, fd(-1)
, img((char*)(const_cast<void*>(base)))
, img_byte_c(4096) /* XXX */
, rela_tab(NULL)
, dynsymtab(NULL)
{
	/* plow through headers */
	switch((elf_arch = ElfImg::getArch(base))) {
	case Arch::ARM:
	case Arch::I386:
		setupTables<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym>();
		break;
	case Arch::X86_64:
		setupTables<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym>();
		break;
	case Arch::Unknown:
		return;
	default:
		assert (0 ==1 && "elf no bueno");
	}
}


ElfDebug::ElfDebug(const char* path)
: is_valid(false)
, fd(-1)
, rela_tab(NULL)
, dynsymtab(NULL)
{
	struct stat	s;
	int		err;

	elf_arch = ElfImg::getArch(path);
	if (elf_arch == Arch::Unknown) return;

	fd = open(path, O_RDONLY);
	if (fd < 0) return;

	err = fstat(fd, &s);
	assert (err == 0 && "Unexpected bad fstat");

	img_byte_c = s.st_size;
	img = (char*)mmap(NULL, img_byte_c, PROT_READ, MAP_SHARED, fd, 0);
	assert (img != MAP_FAILED && "Couldn't map elf ANYWHERE?");

	/* plow through headers */
	switch (elf_arch) {
	case Arch::ARM:
	case Arch::I386:
		setupTables<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym>();
		break;

	case Arch::X86_64:
		setupTables<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym>();
		break;
	default:
		assert (0 ==1 && "elf no bueno");
	}

	is_valid = true;
}

ElfDebug::~ElfDebug(void)
{
	if (!is_valid || fd == -1) {
		return;
	}
	munmap(img, img_byte_c);
	close(fd);
}

template <typename Elf_Ehdr, typename Elf_Shdr, typename Elf_Sym>
void ElfDebug::setupTables(void)
{
	Elf_Ehdr	*hdr;
	Elf_Shdr	*shdr;
	const char	*strtab_sh;

	hdr = (Elf_Ehdr*)img;
	shdr = (Elf_Shdr*)(img + hdr->e_shoff);
	strtab_sh = (const char*)(img + shdr[hdr->e_shstrndx].sh_offset);

	is_exec = (hdr->e_type == ET_EXEC);

	/* pull data from section headers */
	strtab = NULL;
	dynstrtab = NULL;
	dynsymtab = NULL;
	symtab = NULL;
	sym_count = 0;
	rela_tab = NULL;

	for (int i = 0; i < hdr->e_shnum; i++) {
		if (i == hdr->e_shstrndx) continue;

		if (shdr[i].sh_type == SHT_DYNSYM) {
			dynsymtab = (Elf_Sym*)(img + shdr[i].sh_offset);
			dynsym_count = shdr[i].sh_size / shdr[i].sh_entsize;
			continue;
		}

		if (	shdr[i].sh_type == SHT_STRTAB &&
			strcmp(&strtab_sh[shdr[i].sh_name], ".dynstr") == 0)
		{
			dynstrtab = (const char*)(img + shdr[i].sh_offset);
			continue;
		}

		if (shdr[i].sh_type == SHT_STRTAB) {
			strtab = (const char*)(img + shdr[i].sh_offset);
			continue;
		}


		if (shdr[i].sh_type == SHT_SYMTAB) {
			symtab = (void*)(img + shdr[i].sh_offset);
			sym_count = shdr[i].sh_size / shdr[i].sh_entsize;
			assert (sizeof(Elf_Sym) == shdr[i].sh_entsize);
			continue;
		}

		if (	shdr[i].sh_type == SHT_RELA && 
			shdr[i].sh_info == 12 /* XXX ??? */)
		{
			rela_tab = (void*)(img + shdr[i].sh_offset);
			rela_count = shdr[i].sh_size / shdr[i].sh_entsize;
			continue;
		}

	}

	if (symtab == NULL) {
		symtab = dynsymtab;
		sym_count = dynsym_count;
		strtab = dynstrtab;
		is_reloc = false;
	} else {
		is_reloc = true;
	}

	next_sym_idx = 0;
	next_rela_idx = 0;

	/* missing some data we expect; don't try to grab any symbols */
	if (symtab == NULL || strtab == NULL)
		sym_count = 0;
}

std::unique_ptr<Symbol> ElfDebug::nextSym(void)
{
	switch (elf_arch) {
	case Arch::ARM:
	case Arch::I386:
		return nextSym32();
	case Arch::X86_64:
		return nextSym64();
	default:
		assert (0 ==1 && "elf no bueno");
	}

	return NULL;
}

#define NEXTSYM_BITS(x)	\
std::unique_ptr<Symbol> ElfDebug::nextSym##x(void)		\
{	\
	Elf##x##_Sym	*sym = (Elf##x##_Sym*)symtab;	/* FIXME */	\
	Elf##x##_Sym	*cur_sym;	\
	const char	*name_c, *atat;	\
	std::string	name;		\
\
	if (next_sym_idx >= sym_count)	\
		return NULL;		\
\
	cur_sym = &sym[next_sym_idx++];	\
\
	name_c = &strtab[cur_sym->st_name];	\
	name = std::string(name_c);		\
	atat = strstr(name_c, "@@");		\
	if (atat) {				\
		name = name.substr(0, atat - name_c);	\
	}	\
	return std::make_unique<Symbol>(	\
		name,	\
		cur_sym->st_value,	\
		cur_sym->st_size,	\
		is_reloc,	\
		(ELF##x##_ST_TYPE(cur_sym->st_info) == STT_FUNC));	\
}

NEXTSYM_BITS(32)
NEXTSYM_BITS(64)

#define NEXTLINKSYM_BITS(x)	\
std::unique_ptr<Symbol> ElfDebug::nextLinkageSym##x(const GuestMem* m)	\
{	\
	Elf##x##_Sym	*cur_sym;	\
	guest_ptr	guest_sym;	\
	Elf##x##_Sym	*sym = (Elf##x##_Sym*)dynsymtab;	\
	Elf##x##_Rela	*rela;	\
	const char	*name_c;	\
\
	if (!rela_tab || next_rela_idx >= rela_count)\
		return NULL;\
\
	rela = &((Elf##x##_Rela*)rela_tab)[next_rela_idx++];\
	cur_sym = &sym[ELF##x##_R_SYM(rela->r_info)];	\
	name_c = &dynstrtab[cur_sym->st_name];	\
	guest_sym = guest_ptr(rela->r_offset);	\
\
	return std::make_unique<Symbol>(	\
		name_c,	\
		m->read<uint64_t>(guest_sym)-6,	\
		6,	\
		false,	\
		(ELF##x##_ST_TYPE(cur_sym->st_info) == STT_FUNC));	\
}

NEXTLINKSYM_BITS(32)
NEXTLINKSYM_BITS(64)

std::unique_ptr<Symbol> ElfDebug::nextLinkageSym(const GuestMem* m)
{
	switch (elf_arch) {
	case Arch::ARM:
	case Arch::I386:
		return nextLinkageSym32(m);
	case Arch::X86_64:
		return nextLinkageSym64(m);
	default:
		assert (0 ==1 && "elf no bueno");
	}
	return NULL;
}
