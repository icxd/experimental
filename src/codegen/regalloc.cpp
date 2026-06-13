#include <codegen/regalloc.hpp>
#include "ir/ir.hpp"

std::map<std::string, TempAllocation>
allocate_registers(const std::vector<Instruction> &instructions,
                   const std::vector<std::string> &available_registers) {
  auto range_map = compute_live_ranges(instructions);
  auto ranges = to_sorted_ranges(range_map);
  linear_scan_allocate(ranges, available_registers);

  std::map<std::string, TempAllocation> allocations;
  for (const auto &range: ranges) {
    allocations[range.name] =
        TempAllocation{.spilled = range.spilled, .reg = range.assigned_register};
  }

  return allocations;
}

std::map<std::string, LiveRange>
compute_live_ranges(const std::vector<Instruction> &instructions) {
  std::map<std::string, LiveRange> ranges;

  for (int i = 0; i < static_cast<int>(instructions.size()); ++i) {
    const auto &instr = instructions[i];

    for (const auto &src: instr.srcs) {
      if (src.type != OPERAND_TEMPORARY)
        continue;

      std::string name = src.name;
      auto &r = ranges[name];
      r.name = name;
      if (r.start == -1)
        r.start = i;
      r.end = i;
    }

    if (instr.dst && instr.dst->type == OPERAND_TEMPORARY) {
      std::string name = instr.dst->name;
      auto &r = ranges[name];
      r.name = name;
      if (r.start == -1)
        r.start = i;
      r.end = std::max(r.end, i);
    }
  }

  for (int i = 0; i < static_cast<int>(instructions.size()); ++i) {
    if (instructions[i].opcode != OP_CALL)
      continue;
    for (auto &[_, range]: ranges) {
      if (range.start <= i && range.end > i)
        range.crosses_call = true;
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
    for (auto it = active.begin(); it != active.end();) {
      if ((*it)->end < range.start) {
        if (!(*it)->spilled)
          free_regs.push_back((*it)->assigned_register);
        it = active.erase(it);
      } else {
        ++it;
      }
    }

    if (range.crosses_call) {
      range.spilled = true;
      range.assigned_register = "";
      active.push_back(&range);
      continue;
    }

    if (!free_regs.empty()) {
      range.assigned_register = free_regs.back();
      free_regs.pop_back();
      range.spilled = false;
      active.push_back(&range);
      continue;
    }

    LiveRange *spill = &range;
    for (LiveRange *live: active) {
      if (live->end > spill->end)
        spill = live;
    }

    if (spill == &range) {
      range.spilled = true;
      range.assigned_register = "";
      active.push_back(&range);
    } else {
      range.assigned_register = spill->assigned_register;
      range.spilled = false;
      spill->spilled = true;
      spill->assigned_register = "";
      active.push_back(&range);
    }
  }
}
