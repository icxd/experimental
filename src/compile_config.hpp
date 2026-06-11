#pragma once

#include <codegen/codegen.hpp>
#include <common.hpp>
#include <host.hpp>
#include <map>
#include <string>

struct CompileConfig {
  int target_os = HOST_OS;
  int target_arch = HOST_ARCH;
  std::map<std::string, std::string> defines = {};
};

inline ErrorOr<Target> target_for_codegen(const CompileConfig &config) {
  if (config.target_arch == ARCH_X86_64 && config.target_os == 0)
    return TARGET_X86_64_LINUX_GAS;
  if (config.target_arch == ARCH_AARCH64 && config.target_os == 0)
    return TARGET_AARCH64_LINUX_GAS;
  if (config.target_arch == ARCH_X86_64 && config.target_os == 1)
    return TARGET_X86_64_MACOS_GAS;
  if (config.target_arch == ARCH_AARCH64 && config.target_os == 1)
    return TARGET_AARCH64_MACOS_GAS;
  return std::unexpected(
      Error("Unsupported target configuration", 0, 0));
}

inline ErrorOr<void> parse_target_flag(std::string_view triple,
                                       CompileConfig &config) {
  if (triple == "linux-x86_64") {
    config.target_os = 0;
    config.target_arch = ARCH_X86_64;
    return {};
  }
  if (triple == "linux-aarch64") {
    config.target_os = 0;
    config.target_arch = ARCH_AARCH64;
    return {};
  }
  if (triple == "macos-x86_64") {
    config.target_os = 1;
    config.target_arch = ARCH_X86_64;
    return {};
  }
  if (triple == "macos-aarch64") {
    config.target_os = 1;
    config.target_arch = ARCH_AARCH64;
    return {};
  }
  return std::unexpected(
      Error(std::format("Unknown target `{}`", triple), 0, 0)
          .with_hint("Expected one of: linux-x86_64, linux-aarch64, "
                     "macos-x86_64, macos-aarch64"));
}

inline ErrorOr<void> parse_define_flag(std::string_view arg,
                                       CompileConfig &config) {
  std::string name;
  std::string value = "true";

  size_t eq = arg.find('=');
  if (eq == std::string_view::npos) {
    name = std::string(arg);
  } else {
    name = std::string(arg.substr(0, eq));
    value = std::string(arg.substr(eq + 1));
  }

  if (name.empty()) {
    return std::unexpected(
        Error("Expected a name after `-D`", 0, 0)
            .with_hint("Use `-D NAME` or `-D NAME=value`"));
  }

  config.defines[name] = value;
  return {};
}
