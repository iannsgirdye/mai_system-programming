#include "../include/allocator_sorted_list.h"
#include <new>
#include <cstring>
#include <algorithm>

namespace
{
    struct allocator_header
    {
        std::pmr::memory_resource* parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t total_size;
        std::mutex mutex;
        void* first_block;
    };

    struct block_header
    {
        size_t size;
        bool free;
        block_header* next;
    };

    constexpr size_t align_up(size_t value, size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }
}

allocator_sorted_list::allocator_sorted_list(
    size_t space_size,
    std::pmr::memory_resource* parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size < sizeof(allocator_header) + sizeof(block_header))
        throw std::bad_alloc();

    std::pmr::memory_resource* parent =
        parent_allocator ? parent_allocator : std::pmr::get_default_resource();

    void* memory = parent->allocate(space_size, alignof(std::max_align_t));
    if (!memory)
        throw std::bad_alloc();

    this->_trusted_memory = memory;

    auto* header = new (memory) allocator_header;
    header->parent = parent;
    header->mode = allocate_fit_mode;
    header->total_size = space_size;
    header->first_block = nullptr;

    size_t remaining = space_size - sizeof(allocator_header);
    void* block_mem = static_cast<char*>(memory) + sizeof(allocator_header);

    auto* block = new (block_mem) block_header;
    block->size = remaining - sizeof(block_header);
    block->free = true;
    block->next = nullptr;

    header->first_block = block;
}

allocator_sorted_list::~allocator_sorted_list()
{
    if (!_trusted_memory)
        return;

    auto* header = static_cast<allocator_header*>(_trusted_memory);

    std::lock_guard<std::mutex> lock(header->mutex);

    header->~allocator_header();
    header->parent->deallocate(_trusted_memory, header->total_size, alignof(std::max_align_t));

    _trusted_memory = nullptr;
}

allocator_sorted_list::allocator_sorted_list(
    const allocator_sorted_list& other)
{
    throw std::logic_error("copy not supported");
}

allocator_sorted_list& allocator_sorted_list::operator=(
    const allocator_sorted_list& other)
{
    throw std::logic_error("copy not supported");
}

allocator_sorted_list::allocator_sorted_list(
    allocator_sorted_list&& other) noexcept
{
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
}

allocator_sorted_list& allocator_sorted_list::operator=(
    allocator_sorted_list&& other) noexcept
{
    if (this != &other)
    {
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

bool allocator_sorted_list::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept
{
    return this == &other;
}

void* allocator_sorted_list::do_allocate_sm(size_t size)
{
    if (size == 0)
        throw std::bad_alloc();

    auto* header = static_cast<allocator_header*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(header->mutex);

    block_header* best = nullptr;
    block_header* prev_best = nullptr;

    block_header* prev = nullptr;
    block_header* current = static_cast<block_header*>(header->first_block);

    while (current)
    {
        if (current->free && current->size >= size)
        {
            if (header->mode == fit_mode::first_fit)
            {
                best = current;
                prev_best = prev;
                break;
            }

            if (!best)
            {
                best = current;
                prev_best = prev;
            }
            else if (header->mode == fit_mode::the_best_fit)
            {
                if (current->size < best->size)
                {
                    best = current;
                    prev_best = prev;
                }
            }
            else
            {
                if (current->size > best->size)
                {
                    best = current;
                    prev_best = prev;
                }
            }
        }

        prev = current;
        current = current->next;
    }

    if (!best)
        throw std::bad_alloc();

    size_t remaining = best->size;

    if (remaining >= size + sizeof(block_header) + 1)
    {
        auto* new_block = reinterpret_cast<block_header*>(
            reinterpret_cast<char*>(best) + sizeof(block_header) + size);

        new_block->size = remaining - size - sizeof(block_header);
        new_block->free = true;
        new_block->next = best->next;

        best->size = size;
        best->next = new_block;
    }

    best->free = false;

    return reinterpret_cast<void*>(
        reinterpret_cast<char*>(best) + sizeof(block_header));
}

void allocator_sorted_list::do_deallocate_sm(void* at)
{
    if (!at)
        return;

    auto* header = static_cast<allocator_header*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(header->mutex);

    block_header* current = static_cast<block_header*>(header->first_block);
    block_header* prev = nullptr;

    while (current)
    {
        void* user_ptr = reinterpret_cast<void*>(
            reinterpret_cast<char*>(current) + sizeof(block_header));

        if (user_ptr == at)
            break;

        prev = current;
        current = current->next;
    }

    if (!current)
        return;

    current->free = true;

    if (current->next && current->next->free)
    {
        current->size += sizeof(block_header) + current->next->size;
        current->next = current->next->next;
    }

    if (prev && prev->free)
    {
        prev->size += sizeof(block_header) + current->size;
        prev->next = current->next;
    }
}

void allocator_sorted_list::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    auto* header = static_cast<allocator_header*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(header->mutex);
    header->mode = mode;
}

std::vector<allocator_test_utils::block_info>
allocator_sorted_list::get_blocks_info() const noexcept
{
    auto* header = static_cast<allocator_header*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(header->mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info>
allocator_sorted_list::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;

    auto* header = static_cast<allocator_header*>(_trusted_memory);
    block_header* current =
        static_cast<block_header*>(header->first_block);

    while (current)
    {
        result.push_back({current->size, !current->free});
        current = current->next;
    }

    return result;
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator()
    : _free_ptr(nullptr)
{
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void* trusted_memory)
    : _free_ptr(trusted_memory)
{
}

allocator_sorted_list::sorted_iterator::sorted_iterator()
    : _trusted_memory(nullptr)
    , _current_ptr(nullptr)
    , _free_ptr(nullptr)
{
}

allocator_sorted_list::sorted_iterator::sorted_iterator(void* trusted_memory)
    : _trusted_memory(trusted_memory)
    , _current_ptr(nullptr)
    , _free_ptr(nullptr)
{
    if (trusted_memory)
    {
        auto* header = static_cast<allocator_header*>(trusted_memory);
        _current_ptr = reinterpret_cast<block_header*>(header->first_block);
        _free_ptr = _current_ptr;
    }
}

allocator_sorted_list::sorted_free_iterator
allocator_sorted_list::free_begin() const noexcept
{
    return sorted_free_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_free_iterator
allocator_sorted_list::free_end() const noexcept
{
    return sorted_free_iterator(nullptr);
}

allocator_sorted_list::sorted_iterator
allocator_sorted_list::begin() const noexcept
{
    return sorted_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_iterator
allocator_sorted_list::end() const noexcept
{
    return sorted_iterator(nullptr);
}