/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <Kernel/VirtualAddress.h>
#include <LibELF/exec_elf.h>

namespace ELF {

class Image {
public:
    explicit Image(const u8*, size_t, bool verbose_logging = true);
    ~Image();
    void dump() const;
    bool is_valid() const { return m_valid; }
    bool parse();

    bool is_within_image(const void* address, size_t size) const
    {
        if (address < m_buffer)
            return false;
        if (((const u8*)address + size) > m_buffer + m_size)
            return false;
        return true;
    }

    class Section;
    class RelocationSection;
    class Symbol;
    class Relocation;

    class Symbol {
    public:
        Symbol(const Image& image, unsigned index, const Elf32_Sym& sym)
            : m_image(image)
            , m_sym(sym)
            , m_index(index)
        {
        }

        ~Symbol() { }

        StringView name() const { return m_image.table_string(m_sym.st_name); }
        unsigned section_index() const { return m_sym.st_shndx; }
        unsigned value() const { return m_sym.st_value; }
        unsigned size() const { return m_sym.st_size; }
        unsigned index() const { return m_index; }
        unsigned type() const { return ELF32_ST_TYPE(m_sym.st_info); }
        unsigned bind() const { return ELF32_ST_BIND(m_sym.st_info); }
        const Section section() const { return m_image.section(section_index()); }
        StringView raw_data() const;

    private:
        const Image& m_image;
        const Elf32_Sym& m_sym;
        const unsigned m_index;
    };

    class ProgramHeader {
    public:
        ProgramHeader(const Image& image, unsigned program_header_index)
            : m_image(image)
            , m_program_header(image.program_header_internal(program_header_index))
            , m_program_header_index(program_header_index)
        {
        }
        ~ProgramHeader() { }

        unsigned index() const { return m_program_header_index; }
        u32 type() const { return m_program_header.p_type; }
        u32 flags() const { return m_program_header.p_flags; }
        u32 offset() const { return m_program_header.p_offset; }
        VirtualAddress vaddr() const { return VirtualAddress(m_program_header.p_vaddr); }
        u32 size_in_memory() const { return m_program_header.p_memsz; }
        u32 size_in_image() const { return m_program_header.p_filesz; }
        u32 alignment() const { return m_program_header.p_align; }
        bool is_readable() const { return flags() & PF_R; }
        bool is_writable() const { return flags() & PF_W; }
        bool is_executable() const { return flags() & PF_X; }
        const char* raw_data() const { return m_image.raw_data(m_program_header.p_offset); }
        Elf32_Phdr raw_header() const { return m_program_header; }

    private:
        const Image& m_image;
        const Elf32_Phdr& m_program_header;
        unsigned m_program_header_index { 0 };
    };

    class Section {
    public:
        Section(const Image& image, unsigned sectionIndex)
            : m_image(image)
            , m_section_header(image.section_header(sectionIndex))
            , m_section_index(sectionIndex)
        {
        }
        ~Section() { }

        StringView name() const { return m_image.section_header_table_string(m_section_header.sh_name); }
        unsigned type() const { return m_section_header.sh_type; }
        unsigned offset() const { return m_section_header.sh_offset; }
        unsigned size() const { return m_section_header.sh_size; }
        unsigned entry_size() const { return m_section_header.sh_entsize; }
        unsigned entry_count() const { return !entry_size() ? 0 : size() / entry_size(); }
        u32 address() const { return m_section_header.sh_addr; }
        const char* raw_data() const { return m_image.raw_data(m_section_header.sh_offset); }
        ReadonlyBytes bytes() const { return { raw_data(), size() }; }
        bool is_undefined() const { return m_section_index == SHN_UNDEF; }
        const RelocationSection relocations() const;
        u32 flags() const { return m_section_header.sh_flags; }
        bool is_writable() const { return flags() & SHF_WRITE; }
        bool is_executable() const { return flags() & PF_X; }

    protected:
        friend class RelocationSection;
        const Image& m_image;
        const Elf32_Shdr& m_section_header;
        unsigned m_section_index;
    };

    class RelocationSection : public Section {
    public:
        RelocationSection(const Section& section)
            : Section(section.m_image, section.m_section_index)
        {
        }
        unsigned relocation_count() const { return entry_count(); }
        const Relocation relocation(unsigned index) const;
        template<typename F>
        void for_each_relocation(F) const;
    };

    class Relocation {
    public:
        Relocation(const Image& image, const Elf32_Rel& rel)
            : m_image(image)
            , m_rel(rel)
        {
        }

        ~Relocation() { }

        unsigned offset() const { return m_rel.r_offset; }
        unsigned type() const { return ELF32_R_TYPE(m_rel.r_info); }
        unsigned symbol_index() const { return ELF32_R_SYM(m_rel.r_info); }
        const Symbol symbol() const { return m_image.symbol(symbol_index()); }

    private:
        const Image& m_image;
        const Elf32_Rel& m_rel;
    };

    unsigned symbol_count() const;
    unsigned section_count() const;
    unsigned program_header_count() const;

    const Symbol symbol(unsigned) const;
    const Section section(unsigned) const;
    const ProgramHeader program_header(unsigned const) const;
    FlatPtr program_header_table_offset() const;

    template<typename F>
    void for_each_section(F) const;
    template<typename F>
    void for_each_section_of_type(unsigned, F) const;
    template<typename F>
    void for_each_symbol(F) const;
    template<typename F>
    void for_each_program_header(F) const;

    // NOTE: Returns section(0) if section with name is not found.
    // FIXME: I don't love this API.
    const Section lookup_section(const String& name) const;

    bool is_executable() const { return header().e_type == ET_EXEC; }
    bool is_relocatable() const { return header().e_type == ET_REL; }
    bool is_dynamic() const { return header().e_type == ET_DYN; }

    VirtualAddress entry() const { return VirtualAddress(header().e_entry); }
    FlatPtr base_address() const { return (FlatPtr)m_buffer; }
    size_t size() const { return m_size; }

    Optional<Symbol> find_demangled_function(const String& name) const;

    bool has_symbols() const { return symbol_count(); }
    String symbolicate(u32 address, u32* offset = nullptr) const;
    Optional<Image::Symbol> find_symbol(u32 address, u32* offset = nullptr) const;

private:
    const char* raw_data(unsigned offset) const;
    const Elf32_Ehdr& header() const;
    const Elf32_Shdr& section_header(unsigned) const;
    const Elf32_Phdr& program_header_internal(unsigned) const;
    StringView table_string(unsigned offset) const;
    StringView section_header_table_string(unsigned offset) const;
    StringView section_index_to_string(unsigned index) const;
    StringView table_string(unsigned table_index, unsigned offset) const;

    const u8* m_buffer { nullptr };
    size_t m_size { 0 };
    bool m_verbose_logging { true };
    HashMap<String, unsigned> m_sections;
    bool m_valid { false };
    unsigned m_symbol_table_section_index { 0 };
    unsigned m_string_table_section_index { 0 };

    struct SortedSymbol {
        u32 address;
        StringView name;
        String demangled_name;
        Optional<Image::Symbol> symbol;
    };

    mutable Vector<SortedSymbol> m_sorted_symbols;
};

template<typename F>
inline void Image::for_each_section(F func) const
{
    auto section_count = this->section_count();
    for (unsigned i = 0; i < section_count; ++i)
        func(section(i));
}

template<typename F>
inline void Image::for_each_section_of_type(unsigned type, F func) const
{
    auto section_count = this->section_count();
    for (unsigned i = 0; i < section_count; ++i) {
        auto& section = this->section(i);
        if (section.type() == type) {
            if (func(section) == IterationDecision::Break)
                break;
        }
    }
}

template<typename F>
inline void Image::RelocationSection::for_each_relocation(F func) const
{
    auto relocation_count = this->relocation_count();
    for (unsigned i = 0; i < relocation_count; ++i) {
        if (func(relocation(i)) == IterationDecision::Break)
            break;
    }
}

template<typename F>
inline void Image::for_each_symbol(F func) const
{
    auto symbol_count = this->symbol_count();
    for (unsigned i = 0; i < symbol_count; ++i) {
        if (func(symbol(i)) == IterationDecision::Break)
            break;
    }
}

template<typename F>
inline void Image::for_each_program_header(F func) const
{
    auto program_header_count = this->program_header_count();
    for (unsigned i = 0; i < program_header_count; ++i)
        func(program_header(i));
}

} // end namespace ELF
