// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone block-analyzer fixture for test_ppc_aot_lift.py.
#undef NDEBUG
#include <bitset>
#include <cassert>
#include <cstring>
#include <map>
#include <unordered_set>
#include <vector>

#include "Core/PowerPC/PPCAnalyst.h"

static constexpr u32 base = 0x1000;
static std::vector<u32> memory;
static unsigned header_reads;

namespace Core
{
struct System
{
  static System& GetInstance()
  {
    static System system;
    return system;
  }
  System& GetMMU() { return *this; }
  System& GetPowerPC() { return *this; }
  System& GetSymbolDB() { return *this; }
  System& GetBreakPoints() { return *this; }
  int GetMode() const { return 0; }
  bool IsAddressBreakPoint(u32) const { return false; }

  struct Instruction
  {
    bool valid;
    u32 hex, physical_address;
  };
  Instruction TryReadInstruction(u32 address)
  {
    header_reads += address == base;
    const size_t index = (address - base) / 4;
    return {index < memory.size(), index < memory.size() ? memory[index] : 0, address};
  }
};
}  // namespace Core
namespace HLE
{
bool TryReplaceFunction(Core::System&, u32, int) { return false; }
}
namespace Config
{
constexpr bool MAIN_FAST_FORWARD_CTR_IDLE = true, MAIN_RELAXED_IDLE_DETECTION = true;
bool Get(bool setting) { return setting; }
}

const GekkoOPInfo* PPCTables::GetOpInfo(UGeckoInstruction inst, u32)
{
  // One instruction per primary opcode used in these fixtures.
  static const std::map<u32, GekkoOPInfo> entries{
      {11, {"cmpi", OpType::Integer, 1, FL_IN_A | FL_SET_CRn}},
      {14, {"addi", OpType::Integer, 1, FL_OUT_D | FL_IN_A0}},
      {16, {"bcx", OpType::Branch, 1, FL_ENDBLOCK | FL_READ_CR_BI}},
      {18, {"bx", OpType::Branch, 1, FL_ENDBLOCK}},
      {19, {"bclrx", OpType::Branch, 1, FL_ENDBLOCK | FL_READ_CR_BI}},
      {32, {"lwz", OpType::Load, 1, FL_OUT_D | FL_IN_A0 | FL_LOADSTORE}},
  };
  return &entries.at(inst.OPCD);
}

#define DEBUG_ASSERT assert
#include "block_analyzer.inc"

int main()
{
  PPCAnalyst::PPCAnalyzer analyzer;
  const auto scan = [&](size_t limit = 16) {
    header_reads = 0;
    return analyzer.SpanHasIdleLoop(base, memory, limit);
  };

  memory = {0x80640000, 0x2c030000, 0x4182fff8};  // lwz r3,0(r4); cmpwi; beq header
  assert(scan());
  memory[0] = 0x80630000;  // pointer chase reads r3 before writing it
  assert(!scan());
  memory[0] = 0x38840004;  // addi r4,r4,4
  memory.push_back(0x4bfffff4);  // second back-edge to the same header
  assert(!scan() && header_reads == 1);

  memory = {0x2c030000, 0x4bfffffd, 0x4182fff9};  // backward bl and beql
  assert(!scan() && header_reads == 0);
  memory = {0x42000000};  // bdnz to itself
  assert(!scan());
  memory = {0x2c030000, 0x4bfffffd, 0x4182fff8};  // analyze the header via the genuine back-edge
  assert(!scan());  // the earlier linking branch cannot close a strict idle loop
  PPCAnalyst::BlockStats stats{};
  PPCAnalyst::BlockRegStats gpa{}, fpa{};
  PPCAnalyst::CodeBlock block{.m_stats = &stats, .m_gpa = &gpa, .m_fpa = &fpa};
  PPCAnalyst::CodeBuffer ops(3);
  analyzer.Analyze(base, &block, &ops, ops.size());
  assert(block.m_num_instructions == 2 && ops[1].branchIsIdleLoop && !ops[1].branchIsStrictIdleLoop);

  analyzer.SetOption(PPCAnalyst::PPCAnalyzer::OPTION_BRANCH_FOLLOW);
  analyzer.SetBranchFollowingEnabled(true);
  // bl helper; cmpwi; beq header; blr; helper: lwz r3,0(r4); blr
  memory = {0x48000011, 0x2c030000, 0x4182fff8, 0x4e800020, 0x80640000, 0x4e800020};
  assert(!scan(4));
  assert(scan(5));  // the physical span through the back-edge is only three words
}
