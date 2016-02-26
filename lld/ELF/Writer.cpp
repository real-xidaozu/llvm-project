//===- Writer.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "Config.h"
#include "LinkerScript.h"
#include "OutputSections.h"
#include "SymbolTable.h"
#include "Target.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;

using namespace lld;
using namespace lld::elf2;

namespace {
// The writer writes a SymbolTable result to a file.
template <class ELFT> class Writer {
public:
  typedef typename ELFFile<ELFT>::uintX_t uintX_t;
  typedef typename ELFFile<ELFT>::Elf_Shdr Elf_Shdr;
  typedef typename ELFFile<ELFT>::Elf_Ehdr Elf_Ehdr;
  typedef typename ELFFile<ELFT>::Elf_Phdr Elf_Phdr;
  typedef typename ELFFile<ELFT>::Elf_Sym Elf_Sym;
  typedef typename ELFFile<ELFT>::Elf_Sym_Range Elf_Sym_Range;
  typedef typename ELFFile<ELFT>::Elf_Rela Elf_Rela;
  Writer(SymbolTable<ELFT> &S) : Symtab(S) {}
  void run();

private:
  // This describes a program header entry.
  // Each contains type, access flags and range of output sections that will be
  // placed in it.
  struct Phdr {
    Phdr(unsigned Type, unsigned Flags) {
      H.p_type = Type;
      H.p_flags = Flags;
    }
    Elf_Phdr H = {};
    OutputSectionBase<ELFT> *First = nullptr;
    OutputSectionBase<ELFT> *Last = nullptr;
  };

  void copyLocalSymbols();
  void addReservedSymbols();
  bool createSections();
  void addPredefinedSections();
  bool needsGot();

  template <bool isRela>
  void scanRelocs(InputSectionBase<ELFT> &C,
                  iterator_range<const Elf_Rel_Impl<ELFT, isRela> *> Rels);

  void scanRelocs(InputSection<ELFT> &C);
  void scanRelocs(InputSectionBase<ELFT> &S, const Elf_Shdr &RelSec);
  void createPhdrs();
  void assignAddresses();
  void assignAddressesRelocatable();
  void fixAbsoluteSymbols();
  bool openFile();
  void writeHeader();
  void writeSections();
  bool isDiscarded(InputSectionBase<ELFT> *IS) const;
  StringRef getOutputSectionName(InputSectionBase<ELFT> *S) const;
  bool needsInterpSection() const {
    return !Symtab.getSharedFiles().empty() && !Config->DynamicLinker.empty();
  }
  bool isOutputDynamic() const {
    return !Symtab.getSharedFiles().empty() || Config->Shared;
  }

  OutputSection<ELFT> *getBss();
  void addCommonSymbols(std::vector<DefinedCommon *> &Syms);
  void addCopyRelSymbols(std::vector<SharedSymbol<ELFT> *> &Syms);

  std::unique_ptr<llvm::FileOutputBuffer> Buffer;

  BumpPtrAllocator Alloc;
  std::vector<OutputSectionBase<ELFT> *> OutputSections;
  std::vector<std::unique_ptr<OutputSectionBase<ELFT>>> OwningSections;

  // We create a section for the ELF header and one for the program headers.
  ArrayRef<OutputSectionBase<ELFT> *> getSections() const {
    return makeArrayRef(OutputSections).slice(dummySectionsNum());
  }
  unsigned getNumSections() const {
    return OutputSections.size() + 1 - dummySectionsNum();
  }
  // Usually there are 2 dummies sections: ELF header and program header.
  // Relocatable output does not require program headers to be created.
  unsigned dummySectionsNum() const { return Config->Relocatable ? 1 : 2; }

  void addRelIpltSymbols();
  void addStartEndSymbols();
  void addStartStopSymbols(OutputSectionBase<ELFT> *Sec);

  SymbolTable<ELFT> &Symtab;
  std::vector<Phdr> Phdrs;

  uintX_t FileSize;
  uintX_t SectionHeaderOff;

  // Flag to force GOT to be in output if we have relocations
  // that relies on its address.
  bool HasGotOffRel = false;
};
} // anonymous namespace

template <class ELFT> static bool shouldUseRela() { return ELFT::Is64Bits; }

template <class ELFT> void elf2::writeResult(SymbolTable<ELFT> *Symtab) {
  typedef typename ELFFile<ELFT>::uintX_t uintX_t;

  // Create singleton output sections.
  bool IsRela = shouldUseRela<ELFT>();
  DynamicSection<ELFT> Dynamic(*Symtab);
  EhFrameHeader<ELFT> EhFrameHdr;
  GotSection<ELFT> Got;
  InterpSection<ELFT> Interp;
  PltSection<ELFT> Plt;
  RelocationSection<ELFT> RelaDyn(IsRela ? ".rela.dyn" : ".rel.dyn", IsRela);
  StringTableSection<ELFT> DynStrTab(".dynstr", true);
  StringTableSection<ELFT> ShStrTab(".shstrtab", false);
  SymbolTableSection<ELFT> DynSymTab(*Symtab, DynStrTab);

  OutputSectionBase<ELFT> ElfHeader("", 0, SHF_ALLOC);
  OutputSectionBase<ELFT> ProgramHeaders("", 0, SHF_ALLOC);
  ProgramHeaders.updateAlign(sizeof(uintX_t));

  // Instantiate optional output sections if they are needed.
  std::unique_ptr<GnuHashTableSection<ELFT>> GnuHashTab;
  std::unique_ptr<GotPltSection<ELFT>> GotPlt;
  std::unique_ptr<HashTableSection<ELFT>> HashTab;
  std::unique_ptr<RelocationSection<ELFT>> RelaPlt;
  std::unique_ptr<StringTableSection<ELFT>> StrTab;
  std::unique_ptr<SymbolTableSection<ELFT>> SymTabSec;
  std::unique_ptr<OutputSection<ELFT>> MipsRldMap;

  if (Config->GnuHash)
    GnuHashTab.reset(new GnuHashTableSection<ELFT>);
  if (Config->SysvHash)
    HashTab.reset(new HashTableSection<ELFT>);
  if (Target->UseLazyBinding) {
    StringRef S = IsRela ? ".rela.plt" : ".rel.plt";
    GotPlt.reset(new GotPltSection<ELFT>);
    RelaPlt.reset(new RelocationSection<ELFT>(S, IsRela));
  }
  if (!Config->StripAll) {
    StrTab.reset(new StringTableSection<ELFT>(".strtab", false));
    SymTabSec.reset(new SymbolTableSection<ELFT>(*Symtab, *StrTab));
  }
  if (Config->EMachine == EM_MIPS && !Config->Shared) {
    // This is a MIPS specific section to hold a space within the data segment
    // of executable file which is pointed to by the DT_MIPS_RLD_MAP entry.
    // See "Dynamic section" in Chapter 5 in the following document:
    // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
    MipsRldMap.reset(new OutputSection<ELFT>(".rld_map", SHT_PROGBITS,
                                             SHF_ALLOC | SHF_WRITE));
    MipsRldMap->setSize(sizeof(uintX_t));
    MipsRldMap->updateAlign(sizeof(uintX_t));
  }

  Out<ELFT>::DynStrTab = &DynStrTab;
  Out<ELFT>::DynSymTab = &DynSymTab;
  Out<ELFT>::Dynamic = &Dynamic;
  Out<ELFT>::EhFrameHdr = &EhFrameHdr;
  Out<ELFT>::GnuHashTab = GnuHashTab.get();
  Out<ELFT>::Got = &Got;
  Out<ELFT>::GotPlt = GotPlt.get();
  Out<ELFT>::HashTab = HashTab.get();
  Out<ELFT>::Interp = &Interp;
  Out<ELFT>::Plt = &Plt;
  Out<ELFT>::RelaDyn = &RelaDyn;
  Out<ELFT>::RelaPlt = RelaPlt.get();
  Out<ELFT>::ShStrTab = &ShStrTab;
  Out<ELFT>::StrTab = StrTab.get();
  Out<ELFT>::SymTab = SymTabSec.get();
  Out<ELFT>::Bss = nullptr;
  Out<ELFT>::MipsRldMap = MipsRldMap.get();
  Out<ELFT>::Opd = nullptr;
  Out<ELFT>::OpdBuf = nullptr;
  Out<ELFT>::TlsPhdr = nullptr;
  Out<ELFT>::ElfHeader = &ElfHeader;
  Out<ELFT>::ProgramHeaders = &ProgramHeaders;

  Writer<ELFT>(*Symtab).run();
}

// The main function of the writer.
template <class ELFT> void Writer<ELFT>::run() {
  if (!Config->DiscardAll)
    copyLocalSymbols();
  addReservedSymbols();
  if (!createSections())
    return;
  if (!Config->Relocatable) {
    createPhdrs();
    assignAddresses();
  } else {
    assignAddressesRelocatable();
  }
  fixAbsoluteSymbols();
  if (!openFile())
    return;
  writeHeader();
  writeSections();
  if (HasError)
    return;
  fatal(Buffer->commit());
}

namespace {
template <bool Is64Bits> struct SectionKey {
  typedef typename std::conditional<Is64Bits, uint64_t, uint32_t>::type uintX_t;
  StringRef Name;
  uint32_t Type;
  uintX_t Flags;
  uintX_t Alignment;
};
}
namespace llvm {
template <bool Is64Bits> struct DenseMapInfo<SectionKey<Is64Bits>> {
  static SectionKey<Is64Bits> getEmptyKey() {
    return SectionKey<Is64Bits>{DenseMapInfo<StringRef>::getEmptyKey(), 0, 0,
                                0};
  }
  static SectionKey<Is64Bits> getTombstoneKey() {
    return SectionKey<Is64Bits>{DenseMapInfo<StringRef>::getTombstoneKey(), 0,
                                0, 0};
  }
  static unsigned getHashValue(const SectionKey<Is64Bits> &Val) {
    return hash_combine(Val.Name, Val.Type, Val.Flags, Val.Alignment);
  }
  static bool isEqual(const SectionKey<Is64Bits> &LHS,
                      const SectionKey<Is64Bits> &RHS) {
    return DenseMapInfo<StringRef>::isEqual(LHS.Name, RHS.Name) &&
           LHS.Type == RHS.Type && LHS.Flags == RHS.Flags &&
           LHS.Alignment == RHS.Alignment;
  }
};
}

template <class ELFT, class RelT>
static bool handleTlsRelocation(unsigned Type, SymbolBody *Body,
                                InputSectionBase<ELFT> &C, RelT &RI) {
  if (Target->isTlsLocalDynamicRel(Type)) {
    if (Target->canRelaxTls(Type, nullptr))
      return true;
    if (Out<ELFT>::Got->addTlsIndex())
      Out<ELFT>::RelaDyn->addReloc({Target->TlsModuleIndexRel,
                                    DynamicReloc<ELFT>::Off_LTlsIndex,
                                    nullptr});
    return true;
  }

  if (!Body || !Body->IsTls)
    return false;

  if (Target->isTlsGlobalDynamicRel(Type)) {
    if (!Target->canRelaxTls(Type, Body)) {
      if (Out<ELFT>::Got->addDynTlsEntry(Body)) {
        Out<ELFT>::RelaDyn->addReloc({Target->TlsModuleIndexRel,
                                      DynamicReloc<ELFT>::Off_GTlsIndex, Body});
        Out<ELFT>::RelaDyn->addReloc(
            {Target->TlsOffsetRel, DynamicReloc<ELFT>::Off_GTlsOffset, Body});
      }
      return true;
    }
    if (!canBePreempted(Body))
      return true;
  }
  return !Target->isTlsDynRel(Type, *Body);
}

// The reason we have to do this early scan is as follows
// * To mmap the output file, we need to know the size
// * For that, we need to know how many dynamic relocs we will have.
// It might be possible to avoid this by outputting the file with write:
// * Write the allocated output sections, computing addresses.
// * Apply relocations, recording which ones require a dynamic reloc.
// * Write the dynamic relocations.
// * Write the rest of the file.
// This would have some drawbacks. For example, we would only know if .rela.dyn
// is needed after applying relocations. If it is, it will go after rw and rx
// sections. Given that it is ro, we will need an extra PT_LOAD. This
// complicates things for the dynamic linker and means we would have to reserve
// space for the extra PT_LOAD even if we end up not using it.
template <class ELFT>
template <bool isRela>
void Writer<ELFT>::scanRelocs(
    InputSectionBase<ELFT> &C,
    iterator_range<const Elf_Rel_Impl<ELFT, isRela> *> Rels) {
  typedef Elf_Rel_Impl<ELFT, isRela> RelType;
  const ObjectFile<ELFT> &File = *C.getFile();
  for (const RelType &RI : Rels) {
    uint32_t SymIndex = RI.getSymbol(Config->Mips64EL);
    SymbolBody *Body = File.getSymbolBody(SymIndex);
    uint32_t Type = RI.getType(Config->Mips64EL);

    // Ignore "hint" relocation because it is for optional code optimization.
    if (Target->isHintRel(Type))
      continue;

    if (Target->isGotRelative(Type))
      HasGotOffRel = true;

    // Set "used" bit for --as-needed.
    if (Body && Body->isUndefined() && !Body->isWeak())
      if (auto *S = dyn_cast<SharedSymbol<ELFT>>(Body->repl()))
        S->File->IsUsed = true;

    if (Body)
      Body = Body->repl();

    bool CBP = canBePreempted(Body);
    if (handleTlsRelocation<ELFT>(Type, Body, C, RI))
      continue;

    if (Target->needsDynRelative(Type))
      Out<ELFT>::RelaDyn->addReloc({Target->RelativeRel, &C, RI.r_offset, true,
                                    Body, getAddend<ELFT>(RI)});

    // MIPS has a special rule to create GOTs for local symbols.
    if (Config->EMachine == EM_MIPS && !CBP &&
        (Type == R_MIPS_GOT16 || Type == R_MIPS_CALL16)) {
      // FIXME (simon): Do not add so many redundant entries.
      Out<ELFT>::Got->addMipsLocalEntry();
      continue;
    }

    // If a symbol in a DSO is referenced directly instead of through GOT,
    // we need to create a copy relocation for the symbol.
    if (auto *B = dyn_cast_or_null<SharedSymbol<ELFT>>(Body)) {
      if (B->needsCopy())
        continue;
      if (Target->needsCopyRel<ELFT>(Type, *B)) {
        B->NeedsCopyOrPltAddr = true;
        Out<ELFT>::RelaDyn->addReloc(
            {Target->CopyRel, DynamicReloc<ELFT>::Off_Bss, B});
        continue;
      }
    }

    // An STT_GNU_IFUNC symbol always uses a PLT entry, and all references
    // to the symbol go through the PLT. This is true even for a local
    // symbol, although local symbols normally do not require PLT entries.
    if (Body && isGnuIFunc<ELFT>(*Body)) {
      if (Body->isInPlt())
        continue;
      Out<ELFT>::Plt->addEntry(Body);
      if (Target->UseLazyBinding) {
        Out<ELFT>::GotPlt->addEntry(Body);
        Out<ELFT>::RelaPlt->addReloc(
            {CBP ? Target->PltRel : Target->IRelativeRel,
             DynamicReloc<ELFT>::Off_GotPlt, !CBP, Body});
      } else {
        Out<ELFT>::Got->addEntry(Body);
        Out<ELFT>::RelaDyn->addReloc(
            {CBP ? Target->PltRel : Target->IRelativeRel,
             DynamicReloc<ELFT>::Off_Got, !CBP, Body});
      }
      continue;
    }

    // If a relocation needs PLT, we create a PLT and a GOT slot
    // for the symbol.
    TargetInfo::PltNeed NeedPlt = TargetInfo::Plt_No;
    if (Body)
      NeedPlt = Target->needsPlt<ELFT>(Type, *Body);
    if (NeedPlt) {
      if (NeedPlt == TargetInfo::Plt_Implicit)
        Body->NeedsCopyOrPltAddr = true;
      if (Body->isInPlt())
        continue;
      Out<ELFT>::Plt->addEntry(Body);

      if (Target->UseLazyBinding) {
        Out<ELFT>::GotPlt->addEntry(Body);
        Out<ELFT>::RelaPlt->addReloc(
            {Target->PltRel, DynamicReloc<ELFT>::Off_GotPlt, Body});
      } else {
        if (Body->isInGot())
          continue;
        Out<ELFT>::Got->addEntry(Body);
        Out<ELFT>::RelaDyn->addReloc(
            {Target->GotRel, DynamicReloc<ELFT>::Off_Got, Body});
      }
      continue;
    }

    // If a relocation needs GOT, we create a GOT slot for the symbol.
    if (Body && Target->needsGot(Type, *Body)) {
      if (Body->isInGot())
        continue;
      Out<ELFT>::Got->addEntry(Body);

      if (Config->EMachine == EM_MIPS) {
        // MIPS ABI has special rules to process GOT entries
        // and doesn't require relocation entries for them.
        // See "Global Offset Table" in Chapter 5 in the following document
        // for detailed description:
        // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
        Body->MustBeInDynSym = true;
        continue;
      }

      bool Dynrel = Config->Shared && !Target->isRelRelative(Type) &&
                    !Target->isSizeRel(Type);
      if (CBP || Dynrel) {
        uint32_t DynType;
        if (CBP)
          DynType = Body->IsTls ? Target->TlsGotRel : Target->GotRel;
        else
          DynType = Target->RelativeRel;
        Out<ELFT>::RelaDyn->addReloc(
            {DynType, DynamicReloc<ELFT>::Off_Got, !CBP, Body});
      }
      continue;
    }

    if (Config->EMachine == EM_MIPS) {
      if (Type == R_MIPS_LO16)
        // Ignore R_MIPS_LO16 relocation. If it is a pair for R_MIPS_GOT16 we
        // already completed all required action (GOT entry allocation) when
        // handle R_MIPS_GOT16a. If it is a pair for R_MIPS_HI16 against
        // _gp_disp it does not require dynamic relocation. If its a pair for
        // R_MIPS_HI16 against a regular symbol it does not require dynamic
        // relocation too because that case is possible for executable file
        // linking only.
        continue;
      if (Body == Config->MipsGpDisp || Body == Config->MipsLocalGp)
        // MIPS _gp_disp designates offset between start of function and 'gp'
        // pointer into GOT. __gnu_local_gp is equal to the current value of
        // the 'gp'. Therefore any relocations against them do not require
        // dynamic relocation.
        continue;
    }

    if (CBP) {
      // We don't know anything about the finaly symbol. Just ask the dynamic
      // linker to handle the relocation for us.
      Out<ELFT>::RelaDyn->addReloc({Target->getDynRel(Type), &C, RI.r_offset,
                                    false, Body, getAddend<ELFT>(RI)});
      continue;
    }

    // We know that this is the final symbol. If the program being produced
    // is position independent, the final value is still not known.
    // If the relocation depends on the symbol value (not the size or distances
    // in the output), we still need some help from the dynamic linker.
    // We can however do better than just copying the incoming relocation. We
    // can process some of it and and just ask the dynamic linker to add the
    // load address.
    if (!Config->Shared || Target->isRelRelative(Type) ||
        Target->isSizeRel(Type))
      continue;

    uintX_t Addend = getAddend<ELFT>(RI);
    if (Config->EMachine == EM_PPC64 && RI.getType(false) == R_PPC64_TOC) {
      Out<ELFT>::RelaDyn->addReloc({R_PPC64_RELATIVE, &C, RI.r_offset, false,
                                    nullptr,
                                    (uintX_t)getPPC64TocBase() + Addend});
      continue;
    }
    if (Body) {
      Out<ELFT>::RelaDyn->addReloc(
          {Target->RelativeRel, &C, RI.r_offset, true, Body, Addend});
      continue;
    }
    const Elf_Sym *Sym =
        File.getObj().getRelocationSymbol(&RI, File.getSymbolTable());
    InputSectionBase<ELFT> *Section = File.getSection(*Sym);
    uintX_t Offset = Sym->st_value;
    if (Sym->getType() == STT_SECTION) {
      Offset += Addend;
      Addend = 0;
    }
    Out<ELFT>::RelaDyn->addReloc(
        {Target->RelativeRel, &C, RI.r_offset, Section, Offset, Addend});
  }
}

template <class ELFT> void Writer<ELFT>::scanRelocs(InputSection<ELFT> &C) {
  if (C.getSectionHdr()->sh_flags & SHF_ALLOC)
    for (const Elf_Shdr *RelSec : C.RelocSections)
      scanRelocs(C, *RelSec);
}

template <class ELFT>
void Writer<ELFT>::scanRelocs(InputSectionBase<ELFT> &S,
                              const Elf_Shdr &RelSec) {
  ELFFile<ELFT> &EObj = S.getFile()->getObj();
  if (RelSec.sh_type == SHT_RELA)
    scanRelocs(S, EObj.relas(&RelSec));
  else
    scanRelocs(S, EObj.rels(&RelSec));
}

template <class ELFT>
static void reportUndefined(SymbolTable<ELFT> &Symtab, SymbolBody *Sym) {
  if ((Config->Relocatable || Config->Shared) && !Config->NoUndefined)
    return;

  std::string Msg = "undefined symbol: " + Sym->getName().str();
  if (ELFFileBase<ELFT> *File = Symtab.findFile(Sym))
    Msg += " in " + File->getName().str();
  if (Config->NoInhibitExec)
    warning(Msg);
  else
    error(Msg);
}

template <class ELFT>
static bool shouldKeepInSymtab(const ObjectFile<ELFT> &File, StringRef SymName,
                               const typename ELFFile<ELFT>::Elf_Sym &Sym) {
  if (Sym.getType() == STT_SECTION || Sym.getType() == STT_FILE)
    return false;

  InputSectionBase<ELFT> *Sec = File.getSection(Sym);
  // If sym references a section in a discarded group, don't keep it.
  if (Sec == InputSection<ELFT>::Discarded)
    return false;

  if (Config->DiscardNone)
    return true;

  // In ELF assembly .L symbols are normally discarded by the assembler.
  // If the assembler fails to do so, the linker discards them if
  // * --discard-locals is used.
  // * The symbol is in a SHF_MERGE section, which is normally the reason for
  //   the assembler keeping the .L symbol.
  if (!SymName.startswith(".L") && !SymName.empty())
    return true;

  if (Config->DiscardLocals)
    return false;

  return !(Sec->getSectionHdr()->sh_flags & SHF_MERGE);
}

// Local symbols are not in the linker's symbol table. This function scans
// each object file's symbol table to copy local symbols to the output.
template <class ELFT> void Writer<ELFT>::copyLocalSymbols() {
  if (!Out<ELFT>::SymTab)
    return;
  for (const std::unique_ptr<ObjectFile<ELFT>> &F : Symtab.getObjectFiles()) {
    for (const Elf_Sym &Sym : F->getLocalSymbols()) {
      ErrorOr<StringRef> SymNameOrErr = Sym.getName(F->getStringTable());
      fatal(SymNameOrErr);
      StringRef SymName = *SymNameOrErr;
      if (!shouldKeepInSymtab<ELFT>(*F, SymName, Sym))
        continue;
      if (Sym.st_shndx != SHN_ABS) {
        InputSectionBase<ELFT> *Section = F->getSection(Sym);
        if (!Section->Live)
          continue;
      }
      ++Out<ELFT>::SymTab->NumLocals;
      F->KeptLocalSyms.push_back(std::make_pair(
          &Sym, Out<ELFT>::SymTab->StrTabSec.addString(SymName)));
    }
  }
}

// PPC64 has a number of special SHT_PROGBITS+SHF_ALLOC+SHF_WRITE sections that
// we would like to make sure appear is a specific order to maximize their
// coverage by a single signed 16-bit offset from the TOC base pointer.
// Conversely, the special .tocbss section should be first among all SHT_NOBITS
// sections. This will put it next to the loaded special PPC64 sections (and,
// thus, within reach of the TOC base pointer).
static int getPPC64SectionRank(StringRef SectionName) {
  return StringSwitch<int>(SectionName)
           .Case(".tocbss", 0)
           .Case(".branch_lt", 2)
           .Case(".toc", 3)
           .Case(".toc1", 4)
           .Case(".opd", 5)
           .Default(1);
}

template <class ELFT> static bool isRelroSection(OutputSectionBase<ELFT> *Sec) {
  if (!Config->ZRelro)
    return false;
  typename OutputSectionBase<ELFT>::uintX_t Flags = Sec->getFlags();
  if (!(Flags & SHF_ALLOC) || !(Flags & SHF_WRITE))
    return false;
  if (Flags & SHF_TLS)
    return true;
  uint32_t Type = Sec->getType();
  if (Type == SHT_INIT_ARRAY || Type == SHT_FINI_ARRAY ||
      Type == SHT_PREINIT_ARRAY)
    return true;
  if (Sec == Out<ELFT>::GotPlt)
    return Config->ZNow;
  if (Sec == Out<ELFT>::Dynamic || Sec == Out<ELFT>::Got)
    return true;
  StringRef S = Sec->getName();
  return S == ".data.rel.ro" || S == ".ctors" || S == ".dtors" || S == ".jcr" ||
         S == ".eh_frame";
}

// Output section ordering is determined by this function.
template <class ELFT>
static bool compareSections(OutputSectionBase<ELFT> *A,
                            OutputSectionBase<ELFT> *B) {
  typedef typename ELFFile<ELFT>::uintX_t uintX_t;

  int Comp = Script->compareSections(A->getName(), B->getName());
  if (Comp != 0)
    return Comp < 0;

  uintX_t AFlags = A->getFlags();
  uintX_t BFlags = B->getFlags();

  // Allocatable sections go first to reduce the total PT_LOAD size and
  // so debug info doesn't change addresses in actual code.
  bool AIsAlloc = AFlags & SHF_ALLOC;
  bool BIsAlloc = BFlags & SHF_ALLOC;
  if (AIsAlloc != BIsAlloc)
    return AIsAlloc;

  // We don't have any special requirements for the relative order of
  // two non allocatable sections.
  if (!AIsAlloc)
    return false;

  // We want the read only sections first so that they go in the PT_LOAD
  // covering the program headers at the start of the file.
  bool AIsWritable = AFlags & SHF_WRITE;
  bool BIsWritable = BFlags & SHF_WRITE;
  if (AIsWritable != BIsWritable)
    return BIsWritable;

  // For a corresponding reason, put non exec sections first (the program
  // header PT_LOAD is not executable).
  bool AIsExec = AFlags & SHF_EXECINSTR;
  bool BIsExec = BFlags & SHF_EXECINSTR;
  if (AIsExec != BIsExec)
    return BIsExec;

  // If we got here we know that both A and B are in the same PT_LOAD.

  // The TLS initialization block needs to be a single contiguous block in a R/W
  // PT_LOAD, so stick TLS sections directly before R/W sections. The TLS NOBITS
  // sections are placed here as they don't take up virtual address space in the
  // PT_LOAD.
  bool AIsTls = AFlags & SHF_TLS;
  bool BIsTls = BFlags & SHF_TLS;
  if (AIsTls != BIsTls)
    return AIsTls;

  // The next requirement we have is to put nobits sections last. The
  // reason is that the only thing the dynamic linker will see about
  // them is a p_memsz that is larger than p_filesz. Seeing that it
  // zeros the end of the PT_LOAD, so that has to correspond to the
  // nobits sections.
  bool AIsNoBits = A->getType() == SHT_NOBITS;
  bool BIsNoBits = B->getType() == SHT_NOBITS;
  if (AIsNoBits != BIsNoBits)
    return BIsNoBits;

  // We place RelRo section before plain r/w ones.
  bool AIsRelRo = isRelroSection(A);
  bool BIsRelRo = isRelroSection(B);
  if (AIsRelRo != BIsRelRo)
    return AIsRelRo;

  // Some architectures have additional ordering restrictions for sections
  // within the same PT_LOAD.
  if (Config->EMachine == EM_PPC64)
    return getPPC64SectionRank(A->getName()) <
           getPPC64SectionRank(B->getName());

  return false;
}

template <class ELFT> OutputSection<ELFT> *Writer<ELFT>::getBss() {
  if (!Out<ELFT>::Bss) {
    Out<ELFT>::Bss =
        new OutputSection<ELFT>(".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE);
    OwningSections.emplace_back(Out<ELFT>::Bss);
    OutputSections.push_back(Out<ELFT>::Bss);
  }
  return Out<ELFT>::Bss;
}

// Until this function is called, common symbols do not belong to any section.
// This function adds them to end of BSS section.
template <class ELFT>
void Writer<ELFT>::addCommonSymbols(std::vector<DefinedCommon *> &Syms) {
  if (Syms.empty())
    return;

  // Sort the common symbols by alignment as an heuristic to pack them better.
  std::stable_sort(Syms.begin(), Syms.end(),
                   [](const DefinedCommon *A, const DefinedCommon *B) {
                     return A->MaxAlignment > B->MaxAlignment;
                   });

  uintX_t Off = getBss()->getSize();
  for (DefinedCommon *C : Syms) {
    Off = alignTo(Off, C->MaxAlignment);
    C->OffsetInBss = Off;
    Off += C->Size;
  }

  Out<ELFT>::Bss->setSize(Off);
}

// Reserve space in .bss for copy relocations.
template <class ELFT>
void Writer<ELFT>::addCopyRelSymbols(std::vector<SharedSymbol<ELFT> *> &Syms) {
  if (Syms.empty())
    return;
  uintX_t Off = getBss()->getSize();
  for (SharedSymbol<ELFT> *C : Syms) {
    const Elf_Sym &Sym = C->Sym;
    const Elf_Shdr *Sec = C->File->getSection(Sym);
    uintX_t SecAlign = Sec->sh_addralign;
    unsigned TrailingZeros =
        std::min(countTrailingZeros(SecAlign),
                 countTrailingZeros((uintX_t)Sym.st_value));
    uintX_t Align = 1 << TrailingZeros;
    Out<ELFT>::Bss->updateAlign(Align);
    Off = alignTo(Off, Align);
    C->OffsetInBss = Off;
    Off += Sym.st_size;
  }
  Out<ELFT>::Bss->setSize(Off);
}

template <class ELFT>
StringRef Writer<ELFT>::getOutputSectionName(InputSectionBase<ELFT> *S) const {
  StringRef Dest = Script->getOutputSection<ELFT>(S);
  if (!Dest.empty())
    return Dest;

  StringRef Name = S->getSectionName();
  for (StringRef V : {".text.", ".rodata.", ".data.rel.ro.", ".data.", ".bss.",
                      ".init_array.", ".fini_array.", ".ctors.", ".dtors.",
                      ".tbss.", ".gcc_except_table.", ".tdata."})
    if (Name.startswith(V))
      return V.drop_back();
  return Name;
}

template <class ELFT>
void reportDiscarded(InputSectionBase<ELFT> *IS,
                     const std::unique_ptr<ObjectFile<ELFT>> &File) {
  if (!Config->PrintGcSections || !IS || IS->Live)
    return;
  llvm::errs() << "removing unused section from '" << IS->getSectionName()
               << "' in file '" << File->getName() << "'\n";
}

template <class ELFT>
bool Writer<ELFT>::isDiscarded(InputSectionBase<ELFT> *S) const {
  return !S || S == InputSection<ELFT>::Discarded || !S->Live ||
         Script->isDiscarded(S);
}

// The beginning and the ending of .rel[a].plt section are marked
// with __rel[a]_iplt_{start,end} symbols if it is a statically linked
// executable. The runtime needs these symbols in order to resolve
// all IRELATIVE relocs on startup. For dynamic executables, we don't
// need these symbols, since IRELATIVE relocs are resolved through GOT
// and PLT. For details, see http://www.airs.com/blog/archives/403.
template <class ELFT>
void Writer<ELFT>::addRelIpltSymbols() {
  if (isOutputDynamic() || !Out<ELFT>::RelaPlt)
    return;
  bool IsRela = shouldUseRela<ELFT>();

  StringRef S = IsRela ? "__rela_iplt_start" : "__rel_iplt_start";
  if (Symtab.find(S))
    Symtab.addAbsolute(S, ElfSym<ELFT>::RelaIpltStart);

  S = IsRela ? "__rela_iplt_end" : "__rel_iplt_end";
  if (Symtab.find(S))
    Symtab.addAbsolute(S, ElfSym<ELFT>::RelaIpltEnd);
}

template <class ELFT> static bool includeInSymtab(const SymbolBody &B) {
  if (!B.isUsedInRegularObj())
    return false;

  if (auto *D = dyn_cast<DefinedRegular<ELFT>>(&B)) {
    // Don't include synthetic symbols like __init_array_start in every output.
    if (&D->Sym == &ElfSym<ELFT>::Ignored)
      return false;
    // Exclude symbols pointing to garbage-collected sections.
    if (D->Section && !D->Section->Live)
      return false;
  }
  return true;
}

static bool includeInDynsym(const SymbolBody &B) {
  uint8_t V = B.getVisibility();
  if (V != STV_DEFAULT && V != STV_PROTECTED)
    return false;
  if (Config->ExportDynamic || Config->Shared)
    return true;
  return B.MustBeInDynSym;
}

// This class knows how to create an output section for a given
// input section. Output section type is determined by various
// factors, including input section's sh_flags, sh_type and
// linker scripts.
namespace {
template <class ELFT> class OutputSectionFactory {
  typedef typename ELFFile<ELFT>::Elf_Shdr Elf_Shdr;
  typedef typename ELFFile<ELFT>::uintX_t uintX_t;

public:
  std::pair<OutputSectionBase<ELFT> *, bool> create(InputSectionBase<ELFT> *C,
                                                    StringRef OutsecName);

  OutputSectionBase<ELFT> *lookup(StringRef Name, uint32_t Type, uintX_t Flags);

private:
  SectionKey<ELFT::Is64Bits> createKey(InputSectionBase<ELFT> *C,
                                       StringRef OutsecName);

  SmallDenseMap<SectionKey<ELFT::Is64Bits>, OutputSectionBase<ELFT> *> Map;
};
}

template <class ELFT>
std::pair<OutputSectionBase<ELFT> *, bool>
OutputSectionFactory<ELFT>::create(InputSectionBase<ELFT> *C,
                                   StringRef OutsecName) {
  SectionKey<ELFT::Is64Bits> Key = createKey(C, OutsecName);
  OutputSectionBase<ELFT> *&Sec = Map[Key];
  if (Sec)
    return {Sec, false};

  switch (C->SectionKind) {
  case InputSectionBase<ELFT>::Regular:
    Sec = new OutputSection<ELFT>(Key.Name, Key.Type, Key.Flags);
    break;
  case InputSectionBase<ELFT>::EHFrame:
    Sec = new EHOutputSection<ELFT>(Key.Name, Key.Type, Key.Flags);
    break;
  case InputSectionBase<ELFT>::Merge:
    Sec = new MergeOutputSection<ELFT>(Key.Name, Key.Type, Key.Flags,
                                       Key.Alignment);
    break;
  case InputSectionBase<ELFT>::MipsReginfo:
    Sec = new MipsReginfoOutputSection<ELFT>();
    break;
  }
  return {Sec, true};
}

template <class ELFT>
OutputSectionBase<ELFT> *OutputSectionFactory<ELFT>::lookup(StringRef Name,
                                                            uint32_t Type,
                                                            uintX_t Flags) {
  return Map.lookup({Name, Type, Flags, 0});
}

template <class ELFT>
SectionKey<ELFT::Is64Bits>
OutputSectionFactory<ELFT>::createKey(InputSectionBase<ELFT> *C,
                                      StringRef OutsecName) {
  const Elf_Shdr *H = C->getSectionHdr();
  uintX_t Flags = H->sh_flags & ~SHF_GROUP;

  // For SHF_MERGE we create different output sections for each alignment.
  // This makes each output section simple and keeps a single level mapping from
  // input to output.
  uintX_t Alignment = 0;
  if (isa<MergeInputSection<ELFT>>(C)) {
    Alignment = H->sh_addralign;
    if (H->sh_entsize > Alignment)
      Alignment = H->sh_entsize;
  }

  // GNU as can give .eh_frame secion type SHT_PROGBITS or SHT_X86_64_UNWIND
  // depending on the construct. We want to canonicalize it so that
  // there is only one .eh_frame in the end.
  uint32_t Type = H->sh_type;
  if (Type == SHT_PROGBITS && Config->EMachine == EM_X86_64 &&
      isa<EHInputSection<ELFT>>(C))
    Type = SHT_X86_64_UNWIND;

  return SectionKey<ELFT::Is64Bits>{OutsecName, Type, Flags, Alignment};
}

// The linker is expected to define some symbols depending on
// the linking result. This function defines such symbols.
template <class ELFT> void Writer<ELFT>::addReservedSymbols() {
  // __tls_get_addr is defined by the dynamic linker for dynamic ELFs. For
  // static linking the linker is required to optimize away any references to
  // __tls_get_addr, so it's not defined anywhere. Create a hidden definition
  // to avoid the undefined symbol error.
  if (!isOutputDynamic())
    Symtab.addIgnored("__tls_get_addr");

  auto Define = [this](StringRef Name, StringRef Alias, Elf_Sym &Sym) {
    if (Symtab.find(Name))
      Symtab.addAbsolute(Name, Sym);
    if (SymbolBody *B = Symtab.find(Alias))
      if (B->isUndefined())
        Symtab.addAbsolute(Alias, Sym);
  };

  // If the "_end" symbol is referenced, it is expected to point to the address
  // right after the data segment. Usually, this symbol points to the end
  // of .bss section or to the end of .data section if .bss section is absent.
  // We don't know the final address of _end yet, so just add a symbol here,
  // and fix ElfSym<ELFT>::End.st_value later.
  // Define "end" as an alias to "_end" if it is used but not defined.
  // We don't want to define that unconditionally because we don't want to
  // break programs that uses "end" as a regular symbol.
  // The similar history with _etext/etext and _edata/edata:
  // Address of _etext is the first location after the last read-only loadable
  // segment. Address of _edata points to the end of the last non SHT_NOBITS
  // section. That is how gold/bfd do. We update the values for these symbols
  // later, after assigning sections to segments.
  Define("_end", "end", ElfSym<ELFT>::End);
  Define("_etext", "etext", ElfSym<ELFT>::Etext);
  Define("_edata", "edata", ElfSym<ELFT>::Edata);
}

// Sort input sections by section name suffixes for
// __attribute__((init_priority(N))).
template <class ELFT> static void sortInitFini(OutputSectionBase<ELFT> *S) {
  if (S)
    reinterpret_cast<OutputSection<ELFT> *>(S)->sortInitFini();
}

// Sort input sections by the special rule for .ctors and .dtors.
template <class ELFT> static void sortCtorsDtors(OutputSectionBase<ELFT> *S) {
  if (S)
    reinterpret_cast<OutputSection<ELFT> *>(S)->sortCtorsDtors();
}

// Create output section objects and add them to OutputSections.
template <class ELFT> bool Writer<ELFT>::createSections() {
  OutputSections.push_back(Out<ELFT>::ElfHeader);
  if (!Config->Relocatable)
    OutputSections.push_back(Out<ELFT>::ProgramHeaders);

  // Add .interp first because some loaders want to see that section
  // on the first page of the executable file when loaded into memory.
  if (needsInterpSection())
    OutputSections.push_back(Out<ELFT>::Interp);

  // Create output sections for input object file sections.
  std::vector<OutputSectionBase<ELFT> *> RegularSections;
  OutputSectionFactory<ELFT> Factory;
  for (const std::unique_ptr<ObjectFile<ELFT>> &F : Symtab.getObjectFiles()) {
    for (InputSectionBase<ELFT> *C : F->getSections()) {
      if (isDiscarded(C)) {
        reportDiscarded(C, F);
        continue;
      }
      OutputSectionBase<ELFT> *Sec;
      bool IsNew;
      std::tie(Sec, IsNew) = Factory.create(C, getOutputSectionName(C));
      if (IsNew) {
        OwningSections.emplace_back(Sec);
        OutputSections.push_back(Sec);
        RegularSections.push_back(Sec);
      }
      Sec->addSection(C);
    }
  }

  Out<ELFT>::Bss = static_cast<OutputSection<ELFT> *>(
      Factory.lookup(".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE));

  // If we have a .opd section (used under PPC64 for function descriptors),
  // store a pointer to it here so that we can use it later when processing
  // relocations.
  Out<ELFT>::Opd = Factory.lookup(".opd", SHT_PROGBITS, SHF_WRITE | SHF_ALLOC);

  Out<ELFT>::Dynamic->PreInitArraySec = Factory.lookup(
      ".preinit_array", SHT_PREINIT_ARRAY, SHF_WRITE | SHF_ALLOC);
  Out<ELFT>::Dynamic->InitArraySec =
      Factory.lookup(".init_array", SHT_INIT_ARRAY, SHF_WRITE | SHF_ALLOC);
  Out<ELFT>::Dynamic->FiniArraySec =
      Factory.lookup(".fini_array", SHT_FINI_ARRAY, SHF_WRITE | SHF_ALLOC);

  // Sort section contents for __attribute__((init_priority(N)).
  sortInitFini(Out<ELFT>::Dynamic->InitArraySec);
  sortInitFini(Out<ELFT>::Dynamic->FiniArraySec);
  sortCtorsDtors(Factory.lookup(".ctors", SHT_PROGBITS, SHF_WRITE | SHF_ALLOC));
  sortCtorsDtors(Factory.lookup(".dtors", SHT_PROGBITS, SHF_WRITE | SHF_ALLOC));

  // The linker needs to define SECNAME_start, SECNAME_end and SECNAME_stop
  // symbols for sections, so that the runtime can get the start and end
  // addresses of each section by section name. Add such symbols.
  addStartEndSymbols();
  for (OutputSectionBase<ELFT> *Sec : RegularSections)
    addStartStopSymbols(Sec);

  // Define __rel[a]_iplt_{start,end} symbols if needed.
  addRelIpltSymbols();

  // Scan relocations. This must be done after every symbol is declared so that
  // we can correctly decide if a dynamic relocation is needed.
  for (const std::unique_ptr<ObjectFile<ELFT>> &F : Symtab.getObjectFiles()) {
    for (InputSectionBase<ELFT> *C : F->getSections()) {
      if (isDiscarded(C))
        continue;
      if (auto *S = dyn_cast<InputSection<ELFT>>(C))
        scanRelocs(*S);
      else if (auto *S = dyn_cast<EHInputSection<ELFT>>(C))
        if (S->RelocSection)
          scanRelocs(*S, *S->RelocSection);
    }
  }

  // Now that we have defined all possible symbols including linker-
  // synthesized ones. Visit all symbols to give the finishing touches.
  std::vector<DefinedCommon *> CommonSymbols;
  std::vector<SharedSymbol<ELFT> *> CopyRelSymbols;
  for (auto &P : Symtab.getSymbols()) {
    SymbolBody *Body = P.second->Body;
    if (auto *U = dyn_cast<Undefined>(Body))
      if (!U->isWeak() && !U->canKeepUndefined())
        reportUndefined<ELFT>(Symtab, Body);

    if (auto *C = dyn_cast<DefinedCommon>(Body))
      CommonSymbols.push_back(C);
    if (auto *SC = dyn_cast<SharedSymbol<ELFT>>(Body))
      if (SC->needsCopy())
        CopyRelSymbols.push_back(SC);

    if (!includeInSymtab<ELFT>(*Body))
      continue;
    if (Out<ELFT>::SymTab)
      Out<ELFT>::SymTab->addSymbol(Body);

    if (isOutputDynamic() && includeInDynsym(*Body))
      Out<ELFT>::DynSymTab->addSymbol(Body);
  }

  // Do not proceed if there was an undefined symbol.
  if (HasError)
    return false;

  addCommonSymbols(CommonSymbols);
  addCopyRelSymbols(CopyRelSymbols);

  // So far we have added sections from input object files.
  // This function adds linker-created Out<ELFT>::* sections.
  addPredefinedSections();

  std::stable_sort(OutputSections.begin(), OutputSections.end(),
                   compareSections<ELFT>);

  for (unsigned I = dummySectionsNum(), N = OutputSections.size(); I < N; ++I)
    OutputSections[I]->SectionIndex = I + 1 - dummySectionsNum();

  for (OutputSectionBase<ELFT> *Sec : getSections())
    Sec->setSHName(Out<ELFT>::ShStrTab->addString(Sec->getName()));

  // Finalizers fix each section's size.
  // .dynsym is finalized early since that may fill up .gnu.hash.
  if (isOutputDynamic())
    Out<ELFT>::DynSymTab->finalize();

  // Fill other section headers. The dynamic table is finalized
  // at the end because some tags like RELSZ depend on result
  // of finalizing other sections. The dynamic string table is
  // finalized once the .dynamic finalizer has added a few last
  // strings. See DynamicSection::finalize()
  for (OutputSectionBase<ELFT> *Sec : OutputSections)
    if (Sec != Out<ELFT>::DynStrTab && Sec != Out<ELFT>::Dynamic)
      Sec->finalize();

  if (isOutputDynamic())
    Out<ELFT>::Dynamic->finalize();
  return true;
}

template <class ELFT> bool Writer<ELFT>::needsGot() {
  if (!Out<ELFT>::Got->empty())
    return true;

  // We add the .got section to the result for dynamic MIPS target because
  // its address and properties are mentioned in the .dynamic section.
  if (Config->EMachine == EM_MIPS && isOutputDynamic())
    return true;

  // If we have a relocation that is relative to GOT (such as GOTOFFREL),
  // we need to emit a GOT even if it's empty.
  return HasGotOffRel;
}

// This function add Out<ELFT>::* sections to OutputSections.
template <class ELFT> void Writer<ELFT>::addPredefinedSections() {
  auto Add = [&](OutputSectionBase<ELFT> *C) {
    if (C)
      OutputSections.push_back(C);
  };

  // This order is not the same as the final output order
  // because we sort the sections using their attributes below.
  Add(Out<ELFT>::SymTab);
  Add(Out<ELFT>::ShStrTab);
  Add(Out<ELFT>::StrTab);
  if (isOutputDynamic()) {
    Add(Out<ELFT>::DynSymTab);
    Add(Out<ELFT>::GnuHashTab);
    Add(Out<ELFT>::HashTab);
    Add(Out<ELFT>::Dynamic);
    Add(Out<ELFT>::DynStrTab);
    if (Out<ELFT>::RelaDyn->hasRelocs())
      Add(Out<ELFT>::RelaDyn);
    Add(Out<ELFT>::MipsRldMap);
  }

  // We always need to add rel[a].plt to output if it has entries.
  // Even during static linking it can contain R_[*]_IRELATIVE relocations.
  if (Out<ELFT>::RelaPlt && Out<ELFT>::RelaPlt->hasRelocs()) {
    Add(Out<ELFT>::RelaPlt);
    Out<ELFT>::RelaPlt->Static = !isOutputDynamic();
  }

  if (needsGot())
    Add(Out<ELFT>::Got);
  if (Out<ELFT>::GotPlt && !Out<ELFT>::GotPlt->empty())
    Add(Out<ELFT>::GotPlt);
  if (!Out<ELFT>::Plt->empty())
    Add(Out<ELFT>::Plt);
  if (Out<ELFT>::EhFrameHdr->Live)
    Add(Out<ELFT>::EhFrameHdr);
}

// The linker is expected to define SECNAME_start and SECNAME_end
// symbols for a few sections. This function defines them.
template <class ELFT> void Writer<ELFT>::addStartEndSymbols() {
  auto Define = [&](StringRef Start, StringRef End,
                    OutputSectionBase<ELFT> *OS) {
    if (OS) {
      Symtab.addSynthetic(Start, *OS, 0);
      Symtab.addSynthetic(End, *OS, OS->getSize());
    } else {
      Symtab.addIgnored(Start);
      Symtab.addIgnored(End);
    }
  };

  Define("__preinit_array_start", "__preinit_array_end",
         Out<ELFT>::Dynamic->PreInitArraySec);
  Define("__init_array_start", "__init_array_end",
         Out<ELFT>::Dynamic->InitArraySec);
  Define("__fini_array_start", "__fini_array_end",
         Out<ELFT>::Dynamic->FiniArraySec);
}

// If a section name is valid as a C identifier (which is rare because of
// the leading '.'), linkers are expected to define __start_<secname> and
// __stop_<secname> symbols. They are at beginning and end of the section,
// respectively. This is not requested by the ELF standard, but GNU ld and
// gold provide the feature, and used by many programs.
template <class ELFT>
void Writer<ELFT>::addStartStopSymbols(OutputSectionBase<ELFT> *Sec) {
  StringRef S = Sec->getName();
  if (!isValidCIdentifier(S))
    return;
  StringSaver Saver(Alloc);
  StringRef Start = Saver.save("__start_" + S);
  StringRef Stop = Saver.save("__stop_" + S);
  if (SymbolBody *B = Symtab.find(Start))
    if (B->isUndefined())
      Symtab.addSynthetic(Start, *Sec, 0);
  if (SymbolBody *B = Symtab.find(Stop))
    if (B->isUndefined())
      Symtab.addSynthetic(Stop, *Sec, Sec->getSize());
}

template <class ELFT> static bool needsPtLoad(OutputSectionBase<ELFT> *Sec) {
  if (!(Sec->getFlags() & SHF_ALLOC))
    return false;

  // Don't allocate VA space for TLS NOBITS sections. The PT_TLS PHDR is
  // responsible for allocating space for them, not the PT_LOAD that
  // contains the TLS initialization image.
  if (Sec->getFlags() & SHF_TLS && Sec->getType() == SHT_NOBITS)
    return false;
  return true;
}

static uint32_t toPhdrFlags(uint64_t Flags) {
  uint32_t Ret = PF_R;
  if (Flags & SHF_WRITE)
    Ret |= PF_W;
  if (Flags & SHF_EXECINSTR)
    Ret |= PF_X;
  return Ret;
}

/// For AMDGPU we need to use custom segment kinds in order to specify which
/// address space data should be loaded into.
template <class ELFT>
static uint32_t getAmdgpuPhdr(OutputSectionBase<ELFT> *Sec) {
  uint32_t Flags = Sec->getFlags();
  if (Flags & SHF_AMDGPU_HSA_CODE)
    return PT_AMDGPU_HSA_LOAD_CODE_AGENT;
  if ((Flags & SHF_AMDGPU_HSA_GLOBAL) && !(Flags & SHF_AMDGPU_HSA_AGENT))
    return PT_AMDGPU_HSA_LOAD_GLOBAL_PROGRAM;
  return PT_LOAD;
}

// Decide which program headers to create and which sections to include in each
// one.
template <class ELFT> void Writer<ELFT>::createPhdrs() {
  auto AddHdr = [this](unsigned Type, unsigned Flags) {
    return &*Phdrs.emplace(Phdrs.end(), Type, Flags);
  };

  auto AddSec = [](Phdr &Hdr, OutputSectionBase<ELFT> *Sec) {
    Hdr.Last = Sec;
    if (!Hdr.First)
      Hdr.First = Sec;
    Hdr.H.p_align = std::max<uintX_t>(Hdr.H.p_align, Sec->getAlign());
  };

  // The first phdr entry is PT_PHDR which describes the program header itself.
  Phdr &Hdr = *AddHdr(PT_PHDR, PF_R);
  AddSec(Hdr, Out<ELFT>::ProgramHeaders);

  // PT_INTERP must be the second entry if exists.
  if (needsInterpSection()) {
    Phdr &Hdr = *AddHdr(PT_INTERP, toPhdrFlags(Out<ELFT>::Interp->getFlags()));
    AddSec(Hdr, Out<ELFT>::Interp);
  }

  // Add the first PT_LOAD segment for regular output sections.
  uintX_t Flags = PF_R;
  Phdr *Load = AddHdr(PT_LOAD, Flags);
  AddSec(*Load, Out<ELFT>::ElfHeader);

  Phdr TlsHdr(PT_TLS, PF_R);
  Phdr RelRo(PT_GNU_RELRO, PF_R);
  for (OutputSectionBase<ELFT> *Sec : OutputSections) {
    if (!(Sec->getFlags() & SHF_ALLOC))
      break;

    // If we meet TLS section then we create TLS header
    // and put all TLS sections inside for futher use when
    // assign addresses.
    if (Sec->getFlags() & SHF_TLS)
      AddSec(TlsHdr, Sec);

    if (!needsPtLoad<ELFT>(Sec))
      continue;

    // If flags changed then we want new load segment.
    uintX_t NewFlags = toPhdrFlags(Sec->getFlags());
    if (Flags != NewFlags) {
      uint32_t LoadType = (Config->EMachine == EM_AMDGPU) ? getAmdgpuPhdr(Sec)
                                                          : (uint32_t)PT_LOAD;
      Load = AddHdr(LoadType, NewFlags);
      Flags = NewFlags;
    }

    AddSec(*Load, Sec);

    if (isRelroSection(Sec))
      AddSec(RelRo, Sec);
  }

  // Add the TLS segment unless it's empty.
  if (TlsHdr.First)
    Phdrs.push_back(std::move(TlsHdr));

  // Add an entry for .dynamic.
  if (isOutputDynamic()) {
    Phdr &H = *AddHdr(PT_DYNAMIC, toPhdrFlags(Out<ELFT>::Dynamic->getFlags()));
    AddSec(H, Out<ELFT>::Dynamic);
  }

  // PT_GNU_RELRO includes all sections that should be marked as
  // read-only by dynamic linker after proccessing relocations.
  if (RelRo.First)
    Phdrs.push_back(std::move(RelRo));

  // PT_GNU_EH_FRAME is a special section pointing on .eh_frame_hdr.
  if (Out<ELFT>::EhFrameHdr->Live) {
    Phdr &Hdr = *AddHdr(PT_GNU_EH_FRAME,
                        toPhdrFlags(Out<ELFT>::EhFrameHdr->getFlags()));
    AddSec(Hdr, Out<ELFT>::EhFrameHdr);
  }

  // PT_GNU_STACK is a special section to tell the loader to make the
  // pages for the stack non-executable.
  if (!Config->ZExecStack)
    AddHdr(PT_GNU_STACK, PF_R | PF_W);
}

// Used for relocatable output (-r). In this case we create only ELF file
// header, do not create program headers. Also assign of section addresses
// is very straightforward: we just put all sections sequentually to the file.
template <class ELFT> void Writer<ELFT>::assignAddressesRelocatable() {
  Out<ELFT>::ElfHeader->setSize(sizeof(Elf_Ehdr));
  uintX_t FileOff = 0;
  for (OutputSectionBase<ELFT> *Sec : OutputSections) {
    FileOff = alignTo(FileOff, Sec->getAlign());
    Sec->setFileOffset(FileOff);
    FileOff += Sec->getSize();
  }
  SectionHeaderOff = alignTo(FileOff, sizeof(uintX_t));
  FileSize = SectionHeaderOff + getNumSections() * sizeof(Elf_Shdr);
}

// Visits all headers in PhdrTable and assigns the adresses to
// the output sections. Also creates common and special headers.
template <class ELFT> void Writer<ELFT>::assignAddresses() {
  Out<ELFT>::ElfHeader->setSize(sizeof(Elf_Ehdr));
  size_t PhdrSize = sizeof(Elf_Phdr) * Phdrs.size();
  Out<ELFT>::ProgramHeaders->setSize(PhdrSize);

  // The first section of each PT_LOAD and the first section after PT_GNU_RELRO
  // have to be page aligned so that the dynamic linker can set the permissions.
  SmallPtrSet<OutputSectionBase<ELFT> *, 4> PageAlign;
  for (const Phdr &P : Phdrs) {
    if (P.H.p_type == PT_GNU_RELRO) {
      // Find the first section after PT_GNU_RELRO. If it is in a PT_LOAD we
      // have to align it to a page.
      auto I = std::find(OutputSections.begin(), OutputSections.end(), P.Last);
      ++I;
      if (I != OutputSections.end() && needsPtLoad(*I))
        PageAlign.insert(*I);
    }

    if (P.H.p_type == PT_LOAD)
      PageAlign.insert(P.First);
  }

  uintX_t ThreadBssOffset = 0;
  uintX_t VA = Target->getVAStart();
  uintX_t FileOff = 0;

  for (OutputSectionBase<ELFT> *Sec : OutputSections) {
    uintX_t Align = Sec->getAlign();
    if (PageAlign.count(Sec))
      Align = std::max<uintX_t>(Align, Target->PageSize);

    if (Sec->getType() != SHT_NOBITS)
      FileOff = alignTo(FileOff, Align);
    Sec->setFileOffset(FileOff);
    if (Sec->getType() != SHT_NOBITS)
      FileOff += Sec->getSize();

    // We only assign VAs to allocated sections.
    if (needsPtLoad<ELFT>(Sec)) {
      VA = alignTo(VA, Align);
      Sec->setVA(VA);
      VA += Sec->getSize();
    } else if (Sec->getFlags() & SHF_TLS && Sec->getType() == SHT_NOBITS) {
      uintX_t TVA = VA + ThreadBssOffset;
      TVA = alignTo(TVA, Align);
      Sec->setVA(TVA);
      ThreadBssOffset = TVA - VA + Sec->getSize();
    }
  }

  // Add space for section headers.
  SectionHeaderOff = alignTo(FileOff, sizeof(uintX_t));
  FileSize = SectionHeaderOff + getNumSections() * sizeof(Elf_Shdr);

  // Update "_end" and "end" symbols so that they
  // point to the end of the data segment.
  ElfSym<ELFT>::End.st_value = VA;

  for (Phdr &PHdr : Phdrs) {
    Elf_Phdr &H = PHdr.H;
    if (PHdr.First) {
      OutputSectionBase<ELFT> *Last = PHdr.Last;
      H.p_filesz = Last->getFileOff() - PHdr.First->getFileOff();
      if (Last->getType() != SHT_NOBITS)
        H.p_filesz += Last->getSize();
      H.p_memsz = Last->getVA() + Last->getSize() - PHdr.First->getVA();
      H.p_offset = PHdr.First->getFileOff();
      H.p_vaddr = PHdr.First->getVA();
    }
    if (PHdr.H.p_type == PT_LOAD)
      H.p_align = Target->PageSize;
    else if (PHdr.H.p_type == PT_GNU_RELRO)
      H.p_align = 1;
    H.p_paddr = H.p_vaddr;

    // The TLS pointer goes after PT_TLS. At least glibc will align it,
    // so round up the size to make sure the offsets are correct.
    if (PHdr.H.p_type == PT_TLS) {
      Out<ELFT>::TlsPhdr = &H;
      H.p_memsz = alignTo(H.p_memsz, H.p_align);
    }
  }
}

static uint32_t getELFFlags() {
  if (Config->EMachine != EM_MIPS)
    return 0;
  // FIXME: In fact ELF flags depends on ELF flags of input object files
  // and selected emulation. For now just use hard coded values.
  uint32_t V = EF_MIPS_ABI_O32 | EF_MIPS_CPIC | EF_MIPS_ARCH_32R2;
  if (Config->Shared)
    V |= EF_MIPS_PIC;
  return V;
}

template <class ELFT>
static typename ELFFile<ELFT>::uintX_t getEntryAddr() {
  if (Config->EntrySym) {
    if (SymbolBody *B = Config->EntrySym->repl())
      return B->getVA<ELFT>();
    return 0;
  }
  if (Config->EntryAddr != uint64_t(-1))
    return Config->EntryAddr;
  return 0;
}

template <class ELFT> static uint8_t getELFEncoding() {
  if (ELFT::TargetEndianness == llvm::support::little)
    return ELFDATA2LSB;
  return ELFDATA2MSB;
}

static uint16_t getELFType() {
  if (Config->Shared)
    return ET_DYN;
  if (Config->Relocatable)
    return ET_REL;
  return ET_EXEC;
}

// This function is called after we have assigned address and size
// to each section. This function fixes some predefined absolute
// symbol values that depend on section address and size.
template <class ELFT> void Writer<ELFT>::fixAbsoluteSymbols() {
  // Update __rel[a]_iplt_{start,end} symbols so that they point
  // to beginning or ending of .rela.plt section, respectively.
  if (Out<ELFT>::RelaPlt) {
    uintX_t Start = Out<ELFT>::RelaPlt->getVA();
    ElfSym<ELFT>::RelaIpltStart.st_value = Start;
    ElfSym<ELFT>::RelaIpltEnd.st_value = Start + Out<ELFT>::RelaPlt->getSize();
  }

  // Update MIPS _gp absolute symbol so that it points to the static data.
  if (Config->EMachine == EM_MIPS)
    ElfSym<ELFT>::MipsGp.st_value = getMipsGpAddr<ELFT>();

  // _etext points to location after the last read-only loadable segment.
  // _edata points to the end of the last non SHT_NOBITS section.
  for (OutputSectionBase<ELFT> *Sec : OutputSections) {
    if (!(Sec->getFlags() & SHF_ALLOC))
      continue;
    if (!(Sec->getFlags() & SHF_WRITE))
      ElfSym<ELFT>::Etext.st_value = Sec->getVA() + Sec->getSize();
    if (Sec->getType() != SHT_NOBITS)
      ElfSym<ELFT>::Edata.st_value = Sec->getVA() + Sec->getSize();
  }
}

template <class ELFT> void Writer<ELFT>::writeHeader() {
  uint8_t *Buf = Buffer->getBufferStart();
  memcpy(Buf, "\177ELF", 4);

  auto &FirstObj = cast<ELFFileBase<ELFT>>(*Config->FirstElf);

  // Write the ELF header.
  auto *EHdr = reinterpret_cast<Elf_Ehdr *>(Buf);
  EHdr->e_ident[EI_CLASS] = ELFT::Is64Bits ? ELFCLASS64 : ELFCLASS32;
  EHdr->e_ident[EI_DATA] = getELFEncoding<ELFT>();
  EHdr->e_ident[EI_VERSION] = EV_CURRENT;
  EHdr->e_ident[EI_OSABI] = FirstObj.getOSABI();
  EHdr->e_type = getELFType();
  EHdr->e_machine = FirstObj.getEMachine();
  EHdr->e_version = EV_CURRENT;
  EHdr->e_entry = getEntryAddr<ELFT>();
  EHdr->e_shoff = SectionHeaderOff;
  EHdr->e_flags = getELFFlags();
  EHdr->e_ehsize = sizeof(Elf_Ehdr);
  EHdr->e_phnum = Phdrs.size();
  EHdr->e_shentsize = sizeof(Elf_Shdr);
  EHdr->e_shnum = getNumSections();
  EHdr->e_shstrndx = Out<ELFT>::ShStrTab->SectionIndex;

  if (!Config->Relocatable) {
    EHdr->e_phoff = sizeof(Elf_Ehdr);
    EHdr->e_phentsize = sizeof(Elf_Phdr);
  }

  // Write the program header table.
  auto *HBuf = reinterpret_cast<Elf_Phdr *>(Buf + EHdr->e_phoff);
  for (Phdr &P : Phdrs)
    *HBuf++ = P.H;

  // Write the section header table. Note that the first table entry is null.
  auto *SHdrs = reinterpret_cast<Elf_Shdr *>(Buf + EHdr->e_shoff);
  for (OutputSectionBase<ELFT> *Sec : getSections())
    Sec->writeHeaderTo(++SHdrs);
}

template <class ELFT> bool Writer<ELFT>::openFile() {
  ErrorOr<std::unique_ptr<FileOutputBuffer>> BufferOrErr =
      FileOutputBuffer::create(Config->OutputFile, FileSize,
                               FileOutputBuffer::F_executable);
  if (error(BufferOrErr, "failed to open " + Config->OutputFile))
    return false;
  Buffer = std::move(*BufferOrErr);
  return true;
}

// Write section contents to a mmap'ed file.
template <class ELFT> void Writer<ELFT>::writeSections() {
  uint8_t *Buf = Buffer->getBufferStart();

  // PPC64 needs to process relocations in the .opd section before processing
  // relocations in code-containing sections.
  if (OutputSectionBase<ELFT> *Sec = Out<ELFT>::Opd) {
    Out<ELFT>::OpdBuf = Buf + Sec->getFileOff();
    Sec->writeTo(Buf + Sec->getFileOff());
  }

  for (OutputSectionBase<ELFT> *Sec : OutputSections)
    if (Sec != Out<ELFT>::Opd)
      Sec->writeTo(Buf + Sec->getFileOff());
}

template void elf2::writeResult<ELF32LE>(SymbolTable<ELF32LE> *Symtab);
template void elf2::writeResult<ELF32BE>(SymbolTable<ELF32BE> *Symtab);
template void elf2::writeResult<ELF64LE>(SymbolTable<ELF64LE> *Symtab);
template void elf2::writeResult<ELF64BE>(SymbolTable<ELF64BE> *Symtab);
