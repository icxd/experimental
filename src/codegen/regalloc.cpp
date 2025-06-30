#include <codegen/regalloc.hpp>
#include "ir/ir.hpp"

std::map<std::string, std::string>
allocate_registers(const std::vector<Instruction> &instructions,
                   const std::vector<std::string> &available_registers) {
  // 1. Analyze
  auto range_map = compute_live_ranges(instructions);

  // 2. Sort
  auto ranges = to_sorted_ranges(range_map);

  // 3. Allocate registers (this will modify the ranges)
  linear_scan_allocate(ranges, available_registers);

  // 4. Collect final assignments into a map
  std::map<std::string, std::string> allocations;
  for (const auto &range: ranges) {
    allocations[range.name] = range.assigned_register;
  }

  return allocations;
}

std::map<std::string, LiveRange>
compute_live_ranges(const std::vector<Instruction> &instructions) {
  std::map<std::string, LiveRange> ranges;

  for (int i = 0; i < instructions.size(); ++i) {
    const auto &instr = instructions[i];

    // handle src operands (reads/uses)
    for (const auto &src: instr.srcs) {
      if (src.type != OPERAND_TEMPORARY)
        continue;

      std::string name = src.name;

      auto &r = ranges[name];
      r.name = name;
      if (r.start == -1)
        r.start = i;
      r.end = i; // read = live until at least here
    }

    // handle dst operand (writes/defs)
    if (instr.dst && instr.dst->type == OPERAND_TEMPORARY) {
      std::string name = instr.dst->name;

      auto &r = ranges[name];
      r.name = name;
      if (r.start == -1)
        r.start = i;
      r.end = std::max(r.end, i); // conservative: assume still live after
    }
  }

  return ranges;
}

std::vector<LiveRange>
to_sorted_ranges(const std::map<std::string, LiveRange> &map) {
  std::vector<LiveRange> out;
  for (const auto &[_, range]: map)
    out.push_back(range);

  std::sort(out.begin(), out.end(), [](const LiveRange &a, const LiveRange &b) {
    return a.start < b.start;
  });

  return out;
}

void linear_scan_allocate(std::vector<LiveRange> &ranges,
                          const std::vector<std::string> &registers) {
  std::sort(ranges.begin(), ranges.end(),
            [](auto &a, auto &b) { return a.start < b.start; });

  std::vector<LiveRange *> active;
  std::vector<std::string> free_regs = registers;
  std::sort(free_regs.rbegin(), free_regs.rend());

  for (auto &range: ranges) {
    // 1. expire old variables
    for (auto it = active.begin(); it != active.end();) {
      if ((*it)->end < range.start) {
        free_regs.push_back((*it)->assigned_register);
        it = active.erase(it);
      } else {
        ++it;
      }
    }

    // 2. try to assign a register
    if (!free_regs.empty()) {
      std::string reg = free_regs.back(); // lowest register first
      free_regs.pop_back();
      range.assigned_register = reg;
      active.push_back(&range);
    } else {
      range.assigned_register = -1; // spilled
    }
  }
}
