#pragma once

#include <common.hpp>
#include <ir/ir.hpp>

struct LiveRange {
  std::string name;
  int start = -1, end = -1;
  bool spilled = false;
  std::string assigned_register = "";
};

struct TempAllocation {
  bool spilled = false;
  std::string reg;
};

struct AllocationResult {
  std::map<std::string, LiveRange> range_map;
  std::vector<LiveRange> ranges;
  std::map<std::string, TempAllocation> allocations;
};

std::map<std::string, TempAllocation>
allocate_registers(const std::vector<Instruction> &instructions,
                   const std::vector<std::string> &available_registers);

std::map<std::string, LiveRange>
compute_live_ranges(const std::vector<Instruction> &instructions);
std::vector<LiveRange>
to_sorted_ranges(const std::map<std::string, LiveRange> &map);
void linear_scan_allocate(std::vector<LiveRange> &ranges,
                          const std::vector<std::string> &registers);
