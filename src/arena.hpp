#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>

class Arena {
public:
  explicit Arena(size_t initial_capacity = 64 * 1024) :
      _capacity(initial_capacity) {
    _buffers.push_back(std::make_unique<std::byte[]>(_capacity));
    _buffer = _buffers.back().get();
  }

  void *allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
    size_t aligned = align_up(_offset, alignment);
    if (aligned + size > _capacity)
      grow(std::max(_capacity * 2, aligned + size));
    void *ptr = _buffer + aligned;
    _offset = aligned + size;
    return ptr;
  }

  template<typename T, typename... Args>
  T *create(Args &&...args) {
    void *ptr = allocate(sizeof(T), alignof(T));
    return std::construct_at(static_cast<T *>(ptr),
                             std::forward<Args>(args)...);
  }

  void reset() {
    _buffers.clear();
    _capacity = 64 * 1024;
    _buffers.push_back(std::make_unique<std::byte[]>(_capacity));
    _buffer = _buffers.back().get();
    _offset = 0;
  }

private:
  static size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  void grow(size_t new_capacity) {
    _buffers.push_back(std::make_unique<std::byte[]>(new_capacity));
    _buffer = _buffers.back().get();
    _capacity = new_capacity;
    _offset = 0;
  }

  std::vector<std::unique_ptr<std::byte[]>> _buffers;
  std::byte *_buffer;
  size_t _capacity;
  size_t _offset = 0;
};
