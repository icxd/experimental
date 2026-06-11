#pragma once

#include <codegen/codegen.hpp>
#include <stdexcept>

#if defined(__linux__)
constexpr int HOST_OS = 0;
#elif defined(__APPLE__)
constexpr int HOST_OS = 1;
#else
constexpr int HOST_OS = 0;
#endif

inline Target get_host_target() {
#if defined(__aarch64__) || defined(__arm64__)
#  if defined(__linux__)
  return TARGET_AARCH64_LINUX_GAS;
#  elif defined(__APPLE__)
  return TARGET_AARCH64_MACOS_GAS;
#  endif
#elif defined(__x86_64__) || defined(_M_X64)
#  if defined(__linux__)
  return TARGET_X86_64_LINUX_GAS;
#  elif defined(__APPLE__)
  return TARGET_X86_64_MACOS_GAS;
#  endif
#endif
  throw std::runtime_error("unsupported host platform");
}
