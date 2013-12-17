// See LICENSE for license details.

#include "processor.h"
#include "extension.h"
#include "common.h"
#include "config.h"
#include "sim.h"
#include "disasm.h"
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <assert.h>
#include <limits.h>
#include <stdexcept>
#include <algorithm>

processor_t::processor_t(sim_t* _sim, mmu_t* _mmu, uint32_t _id)
  : sim(_sim), mmu(_mmu), ext(NULL), disassembler(new disassembler_t),
    id(_id), run(false), debug(false)
{
  reset(true);
  mmu->set_processor(this);

  #define DECLARE_INSN(name, match, mask) REGISTER_INSN(this, name, match, mask)
  #include "encoding.h"
  #undef DECLARE_INSN
  build_opcode_map();
}

processor_t::~processor_t()
{
}

void state_t::reset()
{
  // the ISA guarantees on boot that the PC is 0x2000 and the the processor
  // is in supervisor mode, and in 64-bit mode, if supported, with traps
  // and virtual memory disabled.
  sr = SR_S | SR_S64;
  pc = 0x2000;

  // the following state is undefined upon boot-up,
  // but we zero it for determinism
  XPR.reset();
  FPR.reset();

  evec = 0;
  epc = 0;
  badvaddr = 0;
  cause = 0;
  pcr_k0 = 0;
  pcr_k1 = 0;
  count = 0;
  compare = 0;
  fflags = 0;
  frm = 0;

  load_reservation = -1;
}

void processor_t::set_debug(bool value)
{
  debug = value;
  if (ext)
    ext->set_debug(value);
}

void processor_t::reset(bool value)
{
  if (run == !value)
    return;
  run = !value;

  state.reset(); // reset the core
  set_pcr(CSR_STATUS, state.sr);

  if (ext)
    ext->reset(); // reset the extension
}

void processor_t::take_interrupt()
{
  uint32_t interrupts = (state.sr & SR_IP) >> SR_IP_SHIFT;
  interrupts &= (state.sr & SR_IM) >> SR_IM_SHIFT;

  if (interrupts && (state.sr & SR_EI))
    for (int i = 0; ; i++, interrupts >>= 1)
      if (interrupts & 1)
        throw trap_t((1ULL << ((state.sr & SR_S64) ? 63 : 31)) + i);
}

void processor_t::step(size_t n)
{
  if(!run)
    return;

  size_t i = 0;
  reg_t npc = state.pc;
  mmu_t* _mmu = mmu;

  try
  {
    take_interrupt();

    // execute_insn fetches and executes one instruction
    #define execute_insn(noisy) \
      do { \
        mmu_t::insn_fetch_t fetch = _mmu->load_insn(npc); \
        if(noisy) disasm(fetch.insn.insn); \
        npc = fetch.func(this, fetch.insn.insn, npc); \
      } while(0)

    
    // special execute_insn  for commit log dumping
#ifdef RISCV_ENABLE_COMMITLOG
    //static disassembler disasmblr; 
    #undef execute_insn 
    #define execute_insn(noisy) \
      do { \
        mmu_t::insn_fetch_t fetch = _mmu->load_insn(npc); \
        if(noisy) disasm(fetch.insn.insn); \
        bool in_spvr = state.sr & SR_S; \
        if (!in_spvr) fprintf(stderr, "\n0x%016" PRIx64 " (0x%08" PRIx32 ") ", npc, fetch.insn.insn.bits()); \
        /*if (!in_spvr) fprintf(stderr, "\n0x%016" PRIx64 " (0x%08" PRIx32 ") %s  ", npc, fetch.insn.insn.bits(), disasmblr.disassemble(fetch.insn.insn).c_str());*/ \
        npc = fetch.func(this, fetch.insn.insn, npc); \
      } while(0)
#endif

    if(debug) for( ; i < n; i++) // print out instructions as we go
      execute_insn(true);
    else 
    {
      // unrolled for speed
      for( ; n > 3 && i < n-3; i+=4)
      {
        execute_insn(false);
        execute_insn(false);
        execute_insn(false);
        execute_insn(false);
      }
      for( ; i < n; i++)
        execute_insn(false);
    }

    state.pc = npc;
  }
  catch(trap_t& t)
  {
    take_trap(npc, t);
  }

  // update timer and possibly register a timer interrupt
  uint32_t old_count = state.count;
  state.count += i;
  if(old_count < state.compare && uint64_t(old_count) + i >= state.compare)
    set_interrupt(IRQ_TIMER, true);
}

void processor_t::take_trap(reg_t pc, trap_t& t)
{
  if (debug)
    fprintf(stderr, "core %3d: exception %s, epc 0x%016" PRIx64 "\n",
            id, t.name(), pc);

  // switch to supervisor, set previous supervisor bit, disable interrupts
  set_pcr(CSR_STATUS, (((state.sr & ~SR_EI) | SR_S) & ~SR_PS & ~SR_PEI) |
                      ((state.sr & SR_S) ? SR_PS : 0) |
                      ((state.sr & SR_EI) ? SR_PEI : 0));

  yield_load_reservation();
  state.cause = t.cause();
  state.epc = pc;
  state.pc = state.evec;

  t.side_effects(&state); // might set badvaddr etc.
}

void processor_t::deliver_ipi()
{
  if (run)
    set_pcr(CSR_CLEAR_IPI, 1);
}

void processor_t::disasm(insn_t insn)
{
  // the disassembler is stateless, so we share it
  fprintf(stderr, "core %3d: 0x%016" PRIx64 " (0x%08" PRIx32 ") %s\n",
          id, state.pc, insn.bits(), disassembler->disassemble(insn).c_str());
}

reg_t processor_t::set_pcr(int which, reg_t val)
{
  reg_t old_pcr = get_pcr(which);

  switch (which)
  {
    case CSR_FFLAGS:
      state.fflags = val & (FSR_AEXC >> FSR_AEXC_SHIFT);
      break;
    case CSR_FRM:
      state.frm = val & (FSR_RD >> FSR_RD_SHIFT);
      break;
    case CSR_FCSR:
      state.fflags = (val & FSR_AEXC) >> FSR_AEXC_SHIFT;
      state.frm = (val & FSR_RD) >> FSR_RD_SHIFT;
      break;
    case CSR_STATUS:
      state.sr = (val & ~SR_IP) | (state.sr & SR_IP);
#ifndef RISCV_ENABLE_64BIT
      state.sr &= ~(SR_S64 | SR_U64);
#endif
#ifndef RISCV_ENABLE_FPU
      state.sr &= ~SR_EF;
#endif
      if (!ext)
        state.sr &= ~SR_EA;
      state.sr &= ~SR_ZERO;
      rv64 = (state.sr & SR_S) ? (state.sr & SR_S64) : (state.sr & SR_U64);
      mmu->flush_tlb();
      break;
    case CSR_EPC:
      state.epc = val;
      break;
    case CSR_EVEC: 
      state.evec = val;
      break;
    case CSR_CYCLE:
    case CSR_TIME:
    case CSR_INSTRET:
    case CSR_COUNT:
      state.count = val;
      break;
    case CSR_COMPARE:
      set_interrupt(IRQ_TIMER, false);
      state.compare = val;
      break;
    case CSR_PTBR:
      state.ptbr = val & ~(PGSIZE-1);
      break;
    case CSR_SEND_IPI:
      sim->send_ipi(val);
      break;
    case CSR_CLEAR_IPI:
      set_interrupt(IRQ_IPI, val & 1);
      break;
    case CSR_SUP0:
      state.pcr_k0 = val;
      break;
    case CSR_SUP1:
      state.pcr_k1 = val;
      break;
    case CSR_TOHOST:
      if (state.tohost == 0)
        state.tohost = val;
      break;
    case CSR_FROMHOST:
      set_interrupt(IRQ_HOST, val != 0);
      state.fromhost = val;
      break;
  }

  return old_pcr;
}

reg_t processor_t::get_pcr(int which)
{
  switch (which)
  {
    case CSR_FFLAGS:
      return state.fflags;
    case CSR_FRM:
      return state.frm;
    case CSR_FCSR:
      return (state.fflags << FSR_AEXC_SHIFT) | (state.frm << FSR_RD_SHIFT);
    case CSR_STATUS:
      return state.sr;
    case CSR_EPC:
      return state.epc;
    case CSR_BADVADDR:
      return state.badvaddr;
    case CSR_EVEC:
      return state.evec;
    case CSR_CYCLE:
    case CSR_TIME:
    case CSR_INSTRET:
    case CSR_COUNT:
      return state.count;
    case CSR_COMPARE:
      return state.compare;
    case CSR_CAUSE:
      return state.cause;
    case CSR_PTBR:
      return state.ptbr;
    case CSR_ASID:
      return 0;
    case CSR_FATC:
      mmu->flush_tlb();
      return 0;
    case CSR_HARTID:
      return id;
    case CSR_IMPL:
      return 1;
    case CSR_SUP0:
      return state.pcr_k0;
    case CSR_SUP1:
      return state.pcr_k1;
    case CSR_TOHOST:
      return state.tohost;
    case CSR_FROMHOST:
      return state.fromhost;
    default:
      return -1;
  }
}

void processor_t::set_interrupt(int which, bool on)
{
  uint32_t mask = (1 << (which + SR_IP_SHIFT)) & SR_IP;
  if (on)
    state.sr |= mask;
  else
    state.sr &= ~mask;
}

reg_t illegal_instruction(processor_t* p, insn_t insn, reg_t pc)
{
  throw trap_illegal_instruction();
}

insn_func_t processor_t::decode_insn(insn_t insn)
{
  size_t mask = opcode_map.size()-1;
  insn_desc_t* desc = opcode_map[insn.bits() & mask]; 

  while ((insn.bits() & desc->mask) != desc->match)
    desc++;

  return rv64 ? desc->rv64 : desc->rv32;
}

void processor_t::register_insn(insn_desc_t desc)
{
  assert(desc.mask & 1);
  instructions.push_back(desc);
}

void processor_t::build_opcode_map()
{
  size_t buckets = -1;
  for (auto& inst : instructions)
    while ((inst.mask & buckets) != buckets)
      buckets /= 2;
  buckets++;

  struct cmp {
    decltype(insn_desc_t::match) mask;
    cmp(decltype(mask) mask) : mask(mask) {}
    bool operator()(const insn_desc_t& lhs, const insn_desc_t& rhs) {
      if ((lhs.match & mask) != (rhs.match & mask))
        return (lhs.match & mask) < (rhs.match & mask);
      return lhs.match < rhs.match;
    }
  };
  std::sort(instructions.begin(), instructions.end(), cmp(buckets-1));

  opcode_map.resize(buckets);
  opcode_store.resize(instructions.size() + 1);

  size_t j = 0;
  for (size_t b = 0, i = 0; b < buckets; b++)
  {
    opcode_map[b] = &opcode_store[j];
    while (i < instructions.size() && b == (instructions[i].match & (buckets-1)))
      opcode_store[j++] = instructions[i++];
  }

  assert(j == opcode_store.size()-1);
  opcode_store[j].match = opcode_store[j].mask = 0;
  opcode_store[j].rv32 = &illegal_instruction;
  opcode_store[j].rv64 = &illegal_instruction;
}

void processor_t::register_extension(extension_t* x)
{
  for (auto insn : x->get_instructions())
    register_insn(insn);
  build_opcode_map();
  for (auto disasm_insn : x->get_disasms())
    disassembler->add_insn(disasm_insn);
  if (ext != NULL)
    throw std::logic_error("only one extension may be registered");
  ext = x;
  x->set_processor(this);
}
