#include "flipper_application_i.h"
#include <furi.h>

#define TAG "fapp-i"

#define ELF_NAME_BUFFER_LEN 32
#define SECTION_OFFSET(e, n) (e.section_table + n * sizeof(Elf32_Shdr))
#define IS_FLAGS_SET(v, m) ((v & m) == m)
#define RESOLVER_THREAD_YIELD_STEP 30

/**************************************************************************************************/
/**************************************** Relocation Cache ****************************************/
/**************************************************************************************************/

static bool
    relocation_cache_get(RelocationAddressCache_t cache, int symEntry, Elf32_Addr* symAddr) {
    Elf32_Addr* addr = RelocationAddressCache_get(cache, symEntry);
    if(addr) {
        *symAddr = *addr;
        return true;
    } else {
        return false;
    }
}

static void
    relocation_cache_put(RelocationAddressCache_t cache, int symEntry, Elf32_Addr symAddr) {
    RelocationAddressCache_set_at(cache, symEntry, symAddr);
}

/**************************************************************************************************/
/********************************************** ELF ***********************************************/
/**************************************************************************************************/

static bool elf_read_string_from_offset(FlipperApplication* fap, off_t offset, string_t name) {
    bool result = false;

    off_t old = storage_file_tell(fap->fd);

    do {
        if(!storage_file_seek(fap->fd, offset, true)) break;

        char buffer[ELF_NAME_BUFFER_LEN + 1];
        buffer[ELF_NAME_BUFFER_LEN] = 0;

        while(true) {
            uint16_t read = storage_file_read(fap->fd, buffer, ELF_NAME_BUFFER_LEN);
            string_cat_str(name, buffer);
            if(strlen(buffer) < ELF_NAME_BUFFER_LEN) {
                result = true;
                break;
            }

            if(storage_file_get_error(fap->fd) != FSE_OK || read == 0) break;
        }

    } while(false);
    storage_file_seek(fap->fd, old, true);

    return result;
}

static bool elf_read_section_name(FlipperApplication* fap, off_t offset, string_t name) {
    return elf_read_string_from_offset(fap, fap->elf.section_table_strings + offset, name);
}

static bool elf_read_symbol_name(FlipperApplication* fap, off_t offset, string_t name) {
    return elf_read_string_from_offset(fap, fap->elf.symbol_table_strings + offset, name);
}

static bool elf_read_section_header(
    FlipperApplication* fap,
    size_t section_idx,
    Elf32_Shdr* section_header) {
    off_t offset = SECTION_OFFSET(fap->elf, section_idx);
    return storage_file_seek(fap->fd, offset, true) &&
           storage_file_read(fap->fd, section_header, sizeof(Elf32_Shdr)) == sizeof(Elf32_Shdr);
}

static bool elf_read_section(
    FlipperApplication* fap,
    size_t section_idx,
    Elf32_Shdr* section_header,
    string_t name) {
    if(!elf_read_section_header(fap, section_idx, section_header)) {
        return false;
    }

    if(section_header->sh_name && !elf_read_section_name(fap, section_header->sh_name, name)) {
        return false;
    }

    return true;
}

static bool elf_read_symbol(FlipperApplication* e, int n, Elf32_Sym* sym, string_t name) {
    bool success = false;
    off_t old = storage_file_tell(e->fd);
    off_t pos = e->elf.symbol_table + n * sizeof(Elf32_Sym);
    if(storage_file_seek(e->fd, pos, true) &&
       storage_file_read(e->fd, sym, sizeof(Elf32_Sym)) == sizeof(Elf32_Sym)) {
        if(sym->st_name)
            success = elf_read_symbol_name(e, sym->st_name, name);
        else {
            Elf32_Shdr shdr;
            success = elf_read_section(e, sym->st_shndx, &shdr, name);
        }
    }
    storage_file_seek(e->fd, old, true);
    return success;
}

static ELFSection* elf_section_of(ELFFile* elf, int index) {
    ELFSectionDict_it_t it;
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
        if(itref->value.sec_idx == index) {
            return &itref->value;
        }
    }

    return NULL;
}

static Elf32_Addr elf_address_of(FlipperApplication* e, Elf32_Sym* sym, const char* sName) {
    if(sym->st_shndx == SHN_UNDEF) {
        Elf32_Addr addr = 0;
        if(e->api_interface->resolver_callback(sName, &addr)) {
            return addr;
        }
    } else {
        ELFSection* symSec = elf_section_of(&e->elf, sym->st_shndx);
        if(symSec) {
            return ((Elf32_Addr)symSec->data) + sym->st_value;
        }
    }
    FURI_LOG_D(TAG, "  Can not find address for symbol %s", sName);
    return ELF_INVALID_ADDRESS;
}

static void elf_relocate_jmp_call(Elf32_Addr relAddr, int type, Elf32_Addr symAddr) {
    UNUSED(type);
    uint16_t upper_insn = ((uint16_t*)relAddr)[0];
    uint16_t lower_insn = ((uint16_t*)relAddr)[1];
    uint32_t S = (upper_insn >> 10) & 1;
    uint32_t J1 = (lower_insn >> 13) & 1;
    uint32_t J2 = (lower_insn >> 11) & 1;

    int32_t offset = (S << 24) | /* S     -> offset[24] */
                     ((~(J1 ^ S) & 1) << 23) | /* J1    -> offset[23] */
                     ((~(J2 ^ S) & 1) << 22) | /* J2    -> offset[22] */
                     ((upper_insn & 0x03ff) << 12) | /* imm10 -> offset[12:21] */
                     ((lower_insn & 0x07ff) << 1); /* imm11 -> offset[1:11] */
    if(offset & 0x01000000) offset -= 0x02000000;

    offset += symAddr - relAddr;

    S = (offset >> 24) & 1;
    J1 = S ^ (~(offset >> 23) & 1);
    J2 = S ^ (~(offset >> 22) & 1);

    upper_insn = ((upper_insn & 0xf800) | (S << 10) | ((offset >> 12) & 0x03ff));
    ((uint16_t*)relAddr)[0] = upper_insn;

    lower_insn = ((lower_insn & 0xd000) | (J1 << 13) | (J2 << 11) | ((offset >> 1) & 0x07ff));
    ((uint16_t*)relAddr)[1] = lower_insn;
}

static bool elf_relocate_symbol(Elf32_Addr relAddr, int type, Elf32_Addr symAddr) {
    switch(type) {
    case R_ARM_ABS32:
        *((uint32_t*)relAddr) += symAddr;
        FURI_LOG_D(TAG, "  R_ARM_ABS32 relocated is 0x%08X", (unsigned int)*((uint32_t*)relAddr));
        break;
    case R_ARM_THM_PC22:
    case R_ARM_THM_JUMP24:
        elf_relocate_jmp_call(relAddr, type, symAddr);
        FURI_LOG_D(
            TAG, "  R_ARM_THM_CALL/JMP relocated is 0x%08X", (unsigned int)*((uint32_t*)relAddr));
        break;
    default:
        FURI_LOG_D(TAG, "  Undefined relocation %d", type);
        return false;
    }
    return true;
}

static const char* elf_reloc_type_to_str(int symt) {
#define STRCASE(name) \
    case name:        \
        return #name;
    switch(symt) {
        STRCASE(R_ARM_NONE)
        STRCASE(R_ARM_ABS32)
        STRCASE(R_ARM_THM_PC22)
        STRCASE(R_ARM_THM_JUMP24)
    default:
        return "R_<unknow>";
    }
#undef STRCASE
}

static bool elf_relocate(FlipperApplication* e, Elf32_Shdr* h, ELFSection* s) {
    if(s->data) {
        Elf32_Rel rel;
        size_t relEntries = h->sh_size / sizeof(rel);
        size_t relCount;
        (void)storage_file_seek(e->fd, h->sh_offset, true);
        FURI_LOG_D(TAG, " Offset   Info     Type             Name");

        int relocate_result = true;
        string_t symbol_name;
        string_init(symbol_name);

        for(relCount = 0; relCount < relEntries; relCount++) {
            if(relCount % RESOLVER_THREAD_YIELD_STEP == 0) {
                FURI_LOG_D(TAG, "  reloc YIELD");
                furi_delay_tick(1);
            }

            if(storage_file_read(e->fd, &rel, sizeof(Elf32_Rel)) != sizeof(Elf32_Rel)) {
                FURI_LOG_E(TAG, "  reloc read fail");
                string_clear(symbol_name);
                return false;
            }

            Elf32_Addr symAddr;

            int symEntry = ELF32_R_SYM(rel.r_info);
            int relType = ELF32_R_TYPE(rel.r_info);
            Elf32_Addr relAddr = ((Elf32_Addr)s->data) + rel.r_offset;

            if(!relocation_cache_get(e->elf.relocation_cache, symEntry, &symAddr)) {
                Elf32_Sym sym;
                string_reset(symbol_name);
                if(!elf_read_symbol(e, symEntry, &sym, symbol_name)) {
                    FURI_LOG_E(TAG, "  symbol read fail");
                    string_clear(symbol_name);
                    return false;
                }

                FURI_LOG_D(
                    TAG,
                    " %08X %08X %-16s %s",
                    (unsigned int)rel.r_offset,
                    (unsigned int)rel.r_info,
                    elf_reloc_type_to_str(relType),
                    string_get_cstr(symbol_name));

                symAddr = elf_address_of(e, &sym, string_get_cstr(symbol_name));
                relocation_cache_put(e->elf.relocation_cache, symEntry, symAddr);
            }

            if(symAddr != ELF_INVALID_ADDRESS) {
                FURI_LOG_D(
                    TAG,
                    "  symAddr=%08X relAddr=%08X",
                    (unsigned int)symAddr,
                    (unsigned int)relAddr);
                if(!elf_relocate_symbol(relAddr, relType, symAddr)) {
                    relocate_result = false;
                }
            } else {
                FURI_LOG_D(TAG, "  No symbol address of %s", symbol_name);
                relocate_result = false;
            }
        }
        string_clear(symbol_name);

        return relocate_result;
    } else {
        FURI_LOG_I(TAG, "Section not loaded");
    }

    return false;
}

/**************************************************************************************************/
/********************************************* MISC ***********************************************/
/**************************************************************************************************/

bool cstr_prefix(const char* prefix, const char* string) {
    return strncmp(prefix, string, strlen(prefix)) == 0;
}

/**************************************************************************************************/
/************************************ Internal FAP interfaces *************************************/
/**************************************************************************************************/
typedef enum {
    SectionTypeERROR = 0,
    SectionTypeSymTab = (1 << 0),
    SectionTypeStrTab = (1 << 2),
    SectionTypeText = (1 << 3),
    SectionTypeRodata = (1 << 4),
    SectionTypeData = (1 << 5),
    SectionTypeBss = (1 << 6),
    SectionTypeRelText = (1 << 7),
    SectionTypeRelRodata = (1 << 8),
    SectionTypeRelData = (1 << 9),
    SectionTypeRelBss = (1 << 10),
    SectionTypeFappManifest = (1 << 11),
    SectionTypeDebugLink = (1 << 12),
    SectionTypeUnused = (1 << 13),
    // TODO add more section types to validate
    SectionTypeValid = SectionTypeSymTab | SectionTypeStrTab | SectionTypeFappManifest,
    SectionTypeRelocate = SectionTypeRelText | SectionTypeRelRodata | SectionTypeRelData |
                          SectionTypeRelBss,
    SectionTypeGdbSection = SectionTypeText | SectionTypeRodata | SectionTypeData | SectionTypeBss,
} SectionType;

static bool
    flipper_application_load_metadata(FlipperApplication* fap, Elf32_Shdr* section_header) {
    if(section_header->sh_size < sizeof(fap->manifest)) {
        return false;
    }

    return storage_file_seek(fap->fd, section_header->sh_offset, true) &&
           storage_file_read(fap->fd, &fap->manifest, section_header->sh_size) ==
               section_header->sh_size;
}

static bool
    flipper_application_load_debug_link(FlipperApplication* fap, Elf32_Shdr* section_header) {
    fap->state.debug_link_size = section_header->sh_size;
    fap->state.debug_link = malloc(section_header->sh_size);

    return storage_file_seek(fap->fd, section_header->sh_offset, true) &&
           storage_file_read(fap->fd, fap->state.debug_link, section_header->sh_size) ==
               section_header->sh_size;
}

static SectionType flipper_application_preload_section(
    FlipperApplication* fap,
    size_t section_idx,
    Elf32_Shdr* section_header,
    string_t name_string) {
    const char* name = string_get_cstr(name_string);

    const struct {
        const char* prefix;
        SectionType type;
    } lookup_sections[] = {
        {".text", SectionTypeText},
        {".rodata", SectionTypeRodata},
        {".data", SectionTypeData},
        {".bss", SectionTypeBss},
        {".rel.text", SectionTypeRelText},
        {".rel.rodata", SectionTypeRelRodata},
        {".rel.data", SectionTypeRelData},
    };

    for(size_t i = 0; i < COUNT_OF(lookup_sections); i++) {
        if(cstr_prefix(lookup_sections[i].prefix, name)) {
            FURI_LOG_D(TAG, "Found section %s", lookup_sections[i].prefix);

            if((lookup_sections[i].type & SectionTypeRelocate) != 0) {
                name = name + strlen(".rel");
            }

            string_t key;
            string_init_set(key, name);
            ELFSection* section_p = ELFSectionDict_get(fap->elf.sections, key);
            if(!section_p) {
                ELFSection section = {
                    .data = NULL,
                    .sec_idx = 0,
                    .rel_sec_idx = 0,
                };

                ELFSectionDict_set_at(fap->elf.sections, key, section);
                section_p = ELFSectionDict_get(fap->elf.sections, key);
            }
            string_clear(key);

            if((lookup_sections[i].type & SectionTypeRelocate) != 0) {
                section_p->rel_sec_idx = section_idx;
            } else {
                section_p->sec_idx = section_idx;
            }

            return lookup_sections[i].type;
        }
    }

    if(strcmp(name, ".symtab") == 0) {
        FURI_LOG_D(TAG, "Found .symtab section");
        fap->elf.symbol_table = section_header->sh_offset;
        fap->elf.symbol_count = section_header->sh_size / sizeof(Elf32_Sym);
        return SectionTypeSymTab;
    } else if(strcmp(name, ".strtab") == 0) {
        FURI_LOG_D(TAG, "Found .strtab section");
        fap->elf.symbol_table_strings = section_header->sh_offset;
        return SectionTypeStrTab;
    } else if(strcmp(name, ".fapmeta") == 0) {
        FURI_LOG_D(TAG, "Found .fapmeta section");
        if(flipper_application_load_metadata(fap, section_header)) {
            return SectionTypeFappManifest;
        } else {
            return SectionTypeERROR;
        }
    } else if(strcmp(name, ".gnu_debuglink") == 0) {
        FURI_LOG_D(TAG, "Found .gnu_debuglink section");
        if(flipper_application_load_debug_link(fap, section_header)) {
            return SectionTypeDebugLink;
        } else {
            return SectionTypeERROR;
        }
    }

    return SectionTypeUnused;
}

static bool flipper_application_load_section_data(FlipperApplication* fap, ELFSection* section) {
    Elf32_Shdr section_header;
    if(section->sec_idx == 0) {
        FURI_LOG_I(TAG, "Section is not present");
        return true;
    }

    if(!elf_read_section_header(fap, section->sec_idx, &section_header)) {
        return false;
    }

    if(section_header.sh_size == 0) {
        FURI_LOG_I(TAG, "No data for section");
        return true;
    }

    section->data = aligned_malloc(section_header.sh_size, section_header.sh_addralign);
    // e->state.mmap_entry_count++;

    if(section_header.sh_type == SHT_NOBITS) {
        /* section is empty (.bss?) */
        /* no need to memset - allocator already did that */
        /* memset(s->data, 0, h->sh_size); */
        FURI_LOG_D(TAG, "0x%X", section->data);
        return true;
    }

    if((!storage_file_seek(fap->fd, section_header.sh_offset, true)) ||
       (storage_file_read(fap->fd, section->data, section_header.sh_size) !=
        section_header.sh_size)) {
        FURI_LOG_E(TAG, "    seek/read fail");
        return false;
    }

    FURI_LOG_D(TAG, "0x%X", section->data);
    return true;
}

static bool flipper_application_relocate_section(FlipperApplication* fap, ELFSection* section) {
    Elf32_Shdr section_header;
    if(section->rel_sec_idx) {
        FURI_LOG_D(TAG, "Relocating section");
        if(elf_read_section_header(fap, section->rel_sec_idx, &section_header))
            return elf_relocate(fap, &section_header, section);
        else {
            FURI_LOG_E(TAG, "Error reading section header");
            return false;
        }
    } else {
        FURI_LOG_D(TAG, "No relocation index"); /* Not an error */
    }
    return true;
}

/**************************************************************************************************/
/************************************ External FAP interfaces *************************************/
/**************************************************************************************************/

bool flipper_application_load_elf_headers(FlipperApplication* fap, const char* path) {
    Elf32_Ehdr h;
    Elf32_Shdr sH;
    ELFFile* elf = &fap->elf;

    if(!storage_file_open(fap->fd, path, FSAM_READ, FSOM_OPEN_EXISTING) ||
       !storage_file_seek(fap->fd, 0, true) ||
       storage_file_read(fap->fd, &h, sizeof(h)) != sizeof(h) ||
       !storage_file_seek(fap->fd, h.e_shoff + h.e_shstrndx * sizeof(sH), true) ||
       storage_file_read(fap->fd, &sH, sizeof(Elf32_Shdr)) != sizeof(Elf32_Shdr)) {
        return false;
    }

    elf->entry = h.e_entry;
    elf->sections_count = h.e_shnum;
    elf->section_table = h.e_shoff;
    elf->section_table_strings = sH.sh_offset;
    return true;
}

bool flipper_application_load_manifest(FlipperApplication* fap) {
    bool result = false;
    ELFFile* elf = &fap->elf;
    string_t name;
    string_init(name);
    ELFSectionDict_init(elf->sections);

    FURI_LOG_D(TAG, "Looking for manifest section");
    for(size_t section_idx = 1; section_idx < elf->sections_count; section_idx++) {
        Elf32_Shdr section_header;

        string_reset(name);
        if(!elf_read_section(fap, section_idx, &section_header, name)) {
            break;
        }

        if(string_cmp(name, ".fapmeta") == 0) {
            if(flipper_application_load_metadata(fap, &section_header)) {
                FURI_LOG_D(TAG, "Load manifest done");
                result = true;
                break;
            } else {
                break;
            }
        }
    }

    string_clear(name);
    return result;
}

bool flipper_application_load_section_table(FlipperApplication* fap) {
    SectionType loaded_sections = SectionTypeERROR;
    ELFFile* elf = &fap->elf;
    string_t name;
    string_init(name);
    ELFSectionDict_init(elf->sections);

    fap->state.mmap_entry_count = 0;

    FURI_LOG_D(TAG, "Scan ELF indexs...");
    for(size_t section_idx = 1; section_idx < elf->sections_count; section_idx++) {
        Elf32_Shdr section_header;

        string_reset(name);
        if(!elf_read_section(fap, section_idx, &section_header, name)) {
            return false;
        }

        FURI_LOG_D(TAG, "Preloading data for section #%d %s", section_idx, string_get_cstr(name));
        SectionType section_type =
            flipper_application_preload_section(fap, section_idx, &section_header, name);
        loaded_sections |= section_type;

        if((section_type & SectionTypeGdbSection) != 0) {
            fap->state.mmap_entry_count++;
        }

        if(section_type == SectionTypeERROR) {
            loaded_sections = SectionTypeERROR;
            break;
        }
    }

    string_clear(name);
    FURI_LOG_D(TAG, "Load symbols done");

    ELFSectionDict_it_t it;
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        const ELFSectionDict_itref_t* itref = ELFSectionDict_cref(it);
        FURI_LOG_D(
            TAG,
            "%s: %d %d",
            string_get_cstr(itref->key),
            itref->value.sec_idx,
            itref->value.rel_sec_idx);
    }

    return IS_FLAGS_SET(loaded_sections, SectionTypeValid);
}

FlipperApplicationLoadStatus flipper_application_load_sections(FlipperApplication* fap) {
    ELFFile* elf = &fap->elf;
    FlipperApplicationLoadStatus status = FlipperApplicationLoadStatusSuccess;
    RelocationAddressCache_init(fap->elf.relocation_cache);
    size_t start = furi_get_tick();

    ELFSectionDict_it_t it;
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
        if(!flipper_application_load_section_data(fap, &itref->value)) {
            FURI_LOG_E(TAG, "Error loading section '%s'", string_get_cstr(itref->key));
            status = FlipperApplicationLoadStatusUnspecifiedError;
        }
    }

    if(status == FlipperApplicationLoadStatusSuccess) {
        for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
            ELFSectionDict_next(it)) {
            ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
            if(!flipper_application_relocate_section(fap, &itref->value)) {
                FURI_LOG_E(TAG, "Error relocating section '%s'", string_get_cstr(itref->key));
                status = FlipperApplicationLoadStatusMissingImports;
            }
        }
    }

    if(status == FlipperApplicationLoadStatusSuccess) {
        FlipperApplicationState* state = &fap->state;
        state->mmap_entries =
            malloc(sizeof(FlipperApplicationMemoryMapEntry) * state->mmap_entry_count);
        uint32_t mmap_entry_idx = 0;
        uint32_t text_p = 0;

        for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
            ELFSectionDict_next(it)) {
            const ELFSectionDict_itref_t* itref = ELFSectionDict_cref(it);

            const void* data_ptr = itref->value.data;
            if(data_ptr) {
                FURI_LOG_I(TAG, "0x%X %s", (uint32_t)data_ptr, string_get_cstr(itref->key));
                state->mmap_entries[mmap_entry_idx].address = (uint32_t)data_ptr;
                state->mmap_entries[mmap_entry_idx].name = string_get_cstr(itref->key);
                mmap_entry_idx++;
            }

            if(string_cmp(itref->key, ".text") == 0) {
                FURI_LOG_I(TAG, "Found .text section at 0x%X", (uint32_t)data_ptr);
                text_p = (uint32_t)data_ptr;
            }
        }

        furi_check(mmap_entry_idx == state->mmap_entry_count);

        /* Fixing up entry point */
        fap->elf.entry += text_p;
    }

    FURI_LOG_D(
        TAG, "Relocation cache size: %u", RelocationAddressCache_size(fap->elf.relocation_cache));
    RelocationAddressCache_clear(fap->elf.relocation_cache);
    FURI_LOG_I(TAG, "Loaded in %ums", (size_t)(furi_get_tick() - start));

    return status;
}

void flipper_application_free_elf_data(ELFFile* elf) {
    ELFSectionDict_it_t it;
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        const ELFSectionDict_itref_t* itref = ELFSectionDict_cref(it);
        if(itref->value.data) {
            aligned_free(itref->value.data);
        }
    }

    ELFSectionDict_clear(elf->sections);
}