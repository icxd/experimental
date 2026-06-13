#pragma once

#include <fcntl.h>
#include <fstream>
#include <unistd.h>

#include <common.hpp>
#include <filesystem>

namespace fs = std::filesystem;

inline fs::path cache_dir() {
  if (const char *env = std::getenv("RYE_CACHE_DIR"))
    return env;
  return fs::current_path() / ".rye";
}

inline void write_file_if_changed(const fs::path &path, std::string_view content) {
  fs::create_directories(path.parent_path());
  if (fs::exists(path)) {
    std::ifstream in(path);
    std::string existing((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    if (existing == content)
      return;
  }
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
  fs::rename(tmp, path);
}

inline bool object_up_to_date(const fs::path &obj, const fs::path &asm_path) {
  if (!fs::exists(obj) || !fs::exists(asm_path))
    return false;
  return fs::last_write_time(obj) >= fs::last_write_time(asm_path);
}

inline bool asm_up_to_date(const fs::path &asm_path, const fs::path &source_path) {
  if (!fs::exists(asm_path) || !fs::exists(source_path))
    return false;
  return fs::last_write_time(asm_path) >= fs::last_write_time(source_path);
}

inline int compile_object(const std::string &obj, const std::string &asm_file) {
  if (object_up_to_date(obj, asm_file))
    return 0;

  std::string lock_path = obj + ".lock";
  int fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd == -1)
    return -1;
  if (flock(fd, LOCK_EX) != 0) {
    close(fd);
    return -1;
  }

  int status = 0;
  if (!object_up_to_date(obj, asm_file))
    status = exec_status("clang -c -o " + obj + " " + asm_file);

  flock(fd, LOCK_UN);
  close(fd);
  return status;
}
