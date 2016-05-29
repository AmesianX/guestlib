#ifndef SYMBOLS_H
#define SYMBOLS_H

#include <stdint.h>
#include <map>
#include <string>
#include <memory>

typedef uintptr_t symaddr_t;

class Symbol
{
public:
	Symbol(	const std::string& in_name, symaddr_t in_addr,
		unsigned int in_len, bool in_dyn = false, bool in_code = false)
	:	name(in_name),
		base_addr(in_addr),
		length(in_len),
		is_dyn(in_dyn),
		is_code(in_code) {}
	virtual ~Symbol() {}
	const std::string& getName() const { return name; }
	symaddr_t getBaseAddr() const { return base_addr; }
	symaddr_t getEndAddr() const { return base_addr + length; }
	unsigned int getLength() const { return length; }
	bool isDynamic(void) const { return is_dyn; }
	bool isCode(void) const { return is_code; }
private:
	std::string	name;
	symaddr_t	base_addr;
	unsigned int	length;
	bool		is_dyn;
	bool		is_code;
};

/* takes a symbol of form (name, address start, size).
 * replies to questions about which symbol contains a given address
 * replies to questions about which name maps to a symbol
 * **assumes no overlaps **
 */

typedef std::map<std::string, Symbol*> symname_map;
typedef std::map<symaddr_t, std::unique_ptr<Symbol>> symaddr_map;

class Symbols
{
public:
	Symbols() {}
	Symbols(const Symbols& s);
	virtual ~Symbols();
	const Symbol* findSym(const std::string& s) const;
	const Symbol* findSym(uint64_t ptr) const;
	bool addSym(
		const std::string& name,
		symaddr_t addr,
		unsigned int len);
	void addSym(const Symbol* sym);
	void addSyms(const Symbols* syms);
	unsigned int size(void) const { return name_map.size(); }

	symname_map::const_iterator begin() const { return name_map.begin(); }
	symname_map::const_iterator end() const { return name_map.end(); }

private:
	symname_map	name_map;
	symaddr_map	addr_map;
};

#endif
