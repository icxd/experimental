#include <rye_paths.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <test_runner.hpp>

namespace fs = std::filesystem;

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdlib.h>
#elif defined(__linux__)
#include <climits>
#include <unistd.h>
#endif

namespace {

bool is_valid_rye_root(const fs::path &root) {
  std::error_code ec;
  return fs::exists(root / "std" / "string.rye", ec) &&
         fs::exists(root / "runtime" / "ryert.rye", ec);
}

std::string canonical_path_string(const fs::path &path) {
  std::error_code ec;
  fs::path canon = fs::weakly_canonical(path, ec);
  return ec ? path.string() : canon.string();
}

void append_unique_path(std::vector<std::string> &paths, const std::string &path) {
  std::string canon = canonical_path_string(path);
  for (const std::string &existing: paths) {
    if (canonical_path_string(existing) == canon)
      return;
  }
  paths.push_back(path);
}

std::optional<std::string> executable_path(std::optional<std::string_view> argv0) {
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (size == 0)
    return std::nullopt;
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    return std::nullopt;
  char resolved[PATH_MAX];
  if (realpath(buffer.c_str(), resolved) == nullptr)
    return canonical_path_string(buffer);
  return std::string(resolved);
#elif defined(__linux__)
  std::string buffer(PATH_MAX, '\0');
  ssize_t length =
      readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) {
    if (!argv0.has_value())
      return std::nullopt;
    return canonical_path_string(fs::path(*argv0));
  }
  buffer.resize(static_cast<size_t>(length));
  return buffer;
#else
  if (!argv0.has_value())
    return std::nullopt;
  return canonical_path_string(fs::path(*argv0));
#endif
}

std::optional<std::string>
root_from_executable(std::optional<std::string_view> argv0) {
  auto exe = executable_path(argv0);
  if (!exe.has_value())
    return std::nullopt;

  fs::path candidate =
      fs::path(*exe).parent_path() / ".." / "share" / "rye";
  std::error_code ec;
  fs::path canon = fs::weakly_canonical(candidate, ec);
  if (!ec && is_valid_rye_root(canon))
    return canon.string();
  return std::nullopt;
}

} // namespace

std::optional<std::string>
rye_install_root(std::optional<std::string_view> argv0) {
  if (const char *prefix = std::getenv("RYE_PREFIX")) {
    fs::path root(prefix);
    if (is_valid_rye_root(root))
      return canonical_path_string(root);
  }

#ifdef RYE_INSTALL_DATADIR
  {
    fs::path compiled(RYE_INSTALL_DATADIR);
    if (is_valid_rye_root(compiled))
      return canonical_path_string(compiled);
  }
#endif

  if (auto from_exe = root_from_executable(argv0))
    return from_exe;

  if (is_valid_rye_root(fs::current_path()))
    return canonical_path_string(fs::current_path());

  return std::nullopt;
}

std::vector<std::string>
rye_builtin_modules(std::optional<std::string_view> argv0,
                    const std::optional<std::string> &dev_root) {
  fs::path root;
  if (dev_root.has_value() && is_valid_rye_root(fs::path(*dev_root)))
    root = fs::path(*dev_root);
  else if (auto install = rye_install_root(argv0))
    root = fs::path(*install);
  else
    root = fs::current_path();

  return {
      canonical_path_string(root / "runtime" / "ryert.rye"),
      canonical_path_string(root / "std" / "string.rye"),
      canonical_path_string(root / "std" / "compiler.rye"),
  };
}

void rye_configure_paths(Opts &opts, std::optional<std::string_view> argv0) {
  append_unique_path(opts.import_paths, fs::current_path().string());

  if (auto root = rye_install_root(argv0))
    append_unique_path(opts.import_paths, *root);

  for (const std::string &module: rye_builtin_modules(argv0))
    opts.files.push_back(module);
}
