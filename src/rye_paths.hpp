#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct Opts;

// Resolve the rye data root (directory containing std/ and runtime/).
std::optional<std::string>
rye_install_root(std::optional<std::string_view> argv0 = std::nullopt);

// Builtin prelude/runtime modules as absolute paths.
std::vector<std::string>
rye_builtin_modules(std::optional<std::string_view> argv0 = std::nullopt,
                    const std::optional<std::string> &dev_root = std::nullopt);

// Add cwd and install root to import search paths; append builtin modules to opts.files.
void rye_configure_paths(Opts &opts, std::optional<std::string_view> argv0);
