#include "../include/allocator_global_heap.h"
#include <mutex>
#include <new>
#include <cstdlib>

static std::mutex mutex;

allocator_global_heap::allocator_global_heap() = default;

allocator_global_heap::~allocator_global_heap() = default;

allocator_global_heap::allocator_global_heap(const allocator_global_heap &other) = default;

allocator_global_heap &allocator_global_heap::operator=(const allocator_global_heap &other) = default;

allocator_global_heap::allocator_global_heap(allocator_global_heap &&other) noexcept = default;

allocator_global_heap &allocator_global_heap::operator=(allocator_global_heap &&other) noexcept = default;

[[nodiscard]] void *allocator_global_heap::do_allocate_sm(size_t size) {
    std::lock_guard<std::mutex> lock(mutex);
    void *ptr = std::malloc(size);
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void allocator_global_heap::do_deallocate_sm(void *at) {
    std::lock_guard<std::mutex> lock(mutex);
    std::free(at);
}

bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
    return dynamic_cast<const allocator_global_heap *>(&other) != nullptr;
}
