#include "../include/allocator_boundary_tags.h"

#include <cstring>
#include <stdexcept>
#include <memory_resource>
#include <mutex>
#include <new>

namespace
{
    struct allocator_header
    {
        std::mutex mtx;
        std::pmr::memory_resource* parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t total_size;
        void* first_occupied;
    };

    struct block_header
    {
        size_t size;
        void* prev;
        void* next;
        void* _;
    };

    constexpr uintptr_t ALLOC_MASK = 1;
    constexpr size_t OCCUPIED_BLOCK_META_SIZE = sizeof(block_header);

    allocator_header& get_header(void* trusted)
    {
        return *static_cast<allocator_header*>(trusted);
    }

    char* block_area_start(void* trusted)
    {
        return static_cast<char*>(trusted) + sizeof(allocator_header);
    }

    char* block_area_end(void* trusted)
    {
        return block_area_start(trusted) + get_header(trusted).total_size;
    }

    block_header* to_block(void* ptr)
    {
        return static_cast<block_header*>(ptr);
    }

    size_t& block_size_field(block_header* blk)
    {
        return blk->size;
    }

    size_t block_size(block_header* blk)
    {
        return block_size_field(blk) & ~ALLOC_MASK;
    }

    bool block_is_occupied(block_header* blk)
    {
        return (block_size_field(blk) & ALLOC_MASK) != 0;
    }

    void block_set_occupied(block_header* blk, size_t full_size)
    {
        block_size_field(blk) = full_size | ALLOC_MASK;
    }
}

allocator_boundary_tags::allocator_boundary_tags(
    size_t space_size,
    std::pmr::memory_resource* parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size < OCCUPIED_BLOCK_META_SIZE)
        throw std::bad_alloc();

    std::pmr::memory_resource* parent =
        parent_allocator ? parent_allocator : std::pmr::get_default_resource();

    void* memory = parent->allocate(sizeof(allocator_header) + space_size, alignof(std::max_align_t));
    if (!memory) throw std::bad_alloc();

    _trusted_memory = memory;

    auto* header = new (memory) allocator_header;
    header->parent = parent;
    header->mode = allocate_fit_mode;
    header->total_size = space_size;
    header->first_occupied = nullptr;
}

allocator_boundary_tags::~allocator_boundary_tags()
{
    if (!_trusted_memory) return;
    auto& header = get_header(_trusted_memory);
    header.mtx.lock();
    std::pmr::memory_resource* parent = header.parent;
    size_t total = sizeof(allocator_header) + header.total_size;
    header.~allocator_header();
    parent->deallocate(_trusted_memory, total, alignof(std::max_align_t));
    _trusted_memory = nullptr;
}

allocator_boundary_tags::allocator_boundary_tags(
    const allocator_boundary_tags& other)
    : _trusted_memory(nullptr)
{
    if (!other._trusted_memory)
        return;

    auto& other_header = get_header(other._trusted_memory);
    std::lock_guard<std::mutex> lock(other_header.mtx);

    size_t total_size = sizeof(allocator_header) + other_header.total_size;
    void* mem = other_header.parent->allocate(total_size, alignof(std::max_align_t));
    if (!mem)
        throw std::bad_alloc();

    std::memcpy(mem, other._trusted_memory, total_size);
    auto* header = new (mem) allocator_header;
    header->parent = other_header.parent;
    header->mode = other_header.mode;
    header->total_size = other_header.total_size;

    if (header->first_occupied)
    {
        ptrdiff_t offset = static_cast<char*>(mem) - static_cast<char*>(other._trusted_memory);
        auto fix_ptr = [&](void*& ptr)
        {
            if (ptr)
                ptr = static_cast<char*>(ptr) + offset;
        };
        for (auto* blk = to_block(header->first_occupied); blk; blk = to_block(blk->next))
        {
            fix_ptr(blk->prev);
            fix_ptr(blk->next);
        }
    }
    _trusted_memory = mem;
}

allocator_boundary_tags& allocator_boundary_tags::operator=(
    const allocator_boundary_tags& other)
{
    if (this == &other)
        return *this;

    this->~allocator_boundary_tags();
    new (this) allocator_boundary_tags(other);
    return *this;
}

allocator_boundary_tags::allocator_boundary_tags(
    allocator_boundary_tags&& other) noexcept
    : _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_boundary_tags& allocator_boundary_tags::operator=(
    allocator_boundary_tags&& other) noexcept
{
    if (this != &other)
    {
        this->~allocator_boundary_tags();
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

void* allocator_boundary_tags::do_allocate_sm(
    size_t size)
{
    if (!_trusted_memory)
        throw std::bad_alloc();

    auto& header = get_header(_trusted_memory);
    std::lock_guard<std::mutex> lock(header.mtx);

    size_t required = size + OCCUPIED_BLOCK_META_SIZE;
    if (required < OCCUPIED_BLOCK_META_SIZE)
        required = OCCUPIED_BLOCK_META_SIZE;

    char* const area_start = block_area_start(_trusted_memory);
    char* const area_end = block_area_end(_trusted_memory);

    void* left = nullptr;
    void* right = header.first_occupied;

    void* best = nullptr;
    size_t best_size = header.mode == allocator_with_fit_mode::fit_mode::the_best_fit
        ? SIZE_MAX
        : 0;

    while (true)
    {
        char* gap_start = left
            ? static_cast<char*>(left) + block_size(to_block(left))
            : area_start;
        char* gap_end = right
            ? static_cast<char*>(right)
            : area_end;
        size_t gap_size = gap_end - gap_start;

        if (gap_size >= required) 
        {
            if (!best)
            {
                best = gap_start;
                best_size = gap_size;
            }
            if (header.mode == allocator_with_fit_mode::fit_mode::first_fit)
                break;
            else if (header.mode == allocator_with_fit_mode::fit_mode::the_best_fit)
            {
                if (gap_size < best_size)
                {
                    best = gap_start;
                    best_size = gap_size;
                }
            }
            else
            {
                if (gap_size > best_size)
                {
                    best = gap_start;
                    best_size = gap_size;
                }
            }
        }

        if (!right)
            break;

        left = right;
        right = to_block(right)->next;
    }

    if (!best)
        throw std::bad_alloc();

    size_t block_size_to_use = required;
    if (best_size < required + OCCUPIED_BLOCK_META_SIZE)
        block_size_to_use = best_size;

    auto* new_block = new (best) block_header;
    block_set_occupied(new_block, block_size_to_use);

    void* prev = nullptr;
    void* next = header.first_occupied;

    while (next && reinterpret_cast<char*>(next) < reinterpret_cast<char*>(new_block))
    {
        prev = next;
        next = to_block(next)->next;
    }

    new_block->prev = prev;
    new_block->next = next;

    if (prev)
        to_block(prev)->next = new_block;
    else
        header.first_occupied = new_block;
    if (next)
        to_block(next)->prev = new_block;

    return reinterpret_cast<char*>(new_block) + OCCUPIED_BLOCK_META_SIZE;
}

void allocator_boundary_tags::do_deallocate_sm(
    void* at)
{
    if (!at || !_trusted_memory)
        return;

    auto& header = get_header(_trusted_memory);
    std::lock_guard<std::mutex> lock(header.mtx);

    char* const area_start = block_area_start(_trusted_memory);
    char* const area_end = block_area_end(_trusted_memory);

    if (static_cast<char*>(at) < area_start + OCCUPIED_BLOCK_META_SIZE ||
        static_cast<char*>(at) > area_end)
        return;

    auto* block = reinterpret_cast<block_header*>(static_cast<char*>(at) - OCCUPIED_BLOCK_META_SIZE);
    if (!block_is_occupied(block))
        return;

    if (block->prev)
        to_block(block->prev)->next = block->next;
    else
        header.first_occupied = block->next;
    if (block->next)
        to_block(block->next)->prev = block->prev;
}

bool allocator_boundary_tags::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept
{
    auto* o = dynamic_cast<const allocator_boundary_tags*>(&other);
    return o != nullptr && o->_trusted_memory == _trusted_memory;
}

void allocator_boundary_tags::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    if (!_trusted_memory)
        return;

    auto& header = get_header(_trusted_memory);
    std::lock_guard<std::mutex> lock(header.mtx);
    header.mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    if (!_trusted_memory)
        return {};

    auto& header = get_header(_trusted_memory);
    std::lock_guard<std::mutex> lock(header.mtx);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;
    if (!_trusted_memory)
        return result;

    char* current = block_area_start(_trusted_memory);
    char* const area_end = block_area_end(_trusted_memory);
    auto* header = &get_header(_trusted_memory);
    auto* next_occupied = static_cast<block_header*>(header->first_occupied);

    while (current < area_end)
    {
        allocator_test_utils::block_info info;
        if (next_occupied && reinterpret_cast<char*>(next_occupied) == current)
        {
            info.block_size = block_size(next_occupied);
            info.is_block_occupied = true;
            current += info.block_size;
            next_occupied = static_cast<block_header*>(next_occupied->next);
        }
        else
        {
            char* free_end = next_occupied
                ? reinterpret_cast<char*>(next_occupied)
                : area_end;
            info.block_size = free_end - current;
            info.is_block_occupied = false;
            current = free_end;
        }
        result.push_back(info);
    }
    return result;
}

allocator_boundary_tags::boundary_iterator::boundary_iterator()
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr)
{}

allocator_boundary_tags::boundary_iterator::boundary_iterator(
    void* trusted)
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(trusted)
{
    if (!trusted)
        return;

    char* start = block_area_start(trusted);
    auto& header = get_header(trusted);
    if (header.first_occupied && start == reinterpret_cast<char*>(header.first_occupied))
    {
        _occupied_ptr = header.first_occupied;
        _occupied = true;
    }
    else
    {
        _occupied_ptr = start;
        _occupied = false;
    }
}

bool allocator_boundary_tags::boundary_iterator::operator==(
    const boundary_iterator& other) const noexcept
{
    return _occupied_ptr == other._occupied_ptr && _occupied == other._occupied;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(
    const boundary_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator&
allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    if (!_trusted_memory || !_occupied_ptr)
        return *this;

    char* const area_end = block_area_end(_trusted_memory);
    char* next_start = nullptr;

    if (_occupied)
    {
        auto* blk = to_block(_occupied_ptr);
        next_start = reinterpret_cast<char*>(blk) + block_size(blk);
    }
    else
    {
        auto& header = get_header(_trusted_memory);
        auto* next_occ = static_cast<block_header*>(header.first_occupied);
        while (next_occ && reinterpret_cast<char*>(next_occ) <= _occupied_ptr)
            next_occ = static_cast<block_header*>(next_occ->next);

        if (next_occ)
            next_start = reinterpret_cast<char*>(next_occ);
        else
            next_start = area_end;
    }

    if (next_start >= area_end)
    {
        _occupied_ptr = nullptr;
        _occupied = false;
        return *this;
    }

    auto& header = get_header(_trusted_memory);
    auto* occ = static_cast<block_header*>(header.first_occupied);
    while (occ && reinterpret_cast<char*>(occ) < next_start)
        occ = static_cast<block_header*>(occ->next);

    if (occ && reinterpret_cast<char*>(occ) == next_start)
    {
        _occupied_ptr = occ;
        _occupied = true;
    }
    else
    {
        _occupied_ptr = next_start;
        _occupied = false;
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator&
allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (!_trusted_memory)
        return *this;

    char* const area_start = block_area_start(_trusted_memory);
    char* const area_end = block_area_end(_trusted_memory);

    if (!_occupied_ptr)
    {
        auto& header = get_header(_trusted_memory);
        if (header.first_occupied)
        {
            auto* last = to_block(header.first_occupied);
            while (last->next)
                last = to_block(last->next);

            char* after_last = reinterpret_cast<char*>(last) + block_size(last);
            if (after_last < area_end)
            {
                _occupied_ptr = after_last;
                _occupied = false;
            }
            else
            {
                _occupied_ptr = last;
                _occupied = true;
            }
        }
        else
        {
            _occupied_ptr = area_start;
            _occupied = false;
        }
        return *this;
    }

    char* current_start = reinterpret_cast<char*>(_occupied_ptr);
    auto& header = get_header(_trusted_memory);
    void* prev_occupied = nullptr;
    for (auto* cur = static_cast<block_header*>(header.first_occupied); cur; cur = to_block(cur->next))
    {
        if (reinterpret_cast<char*>(cur) < current_start)
            prev_occupied = cur;
        else
            break;
    }

    if (prev_occupied)
    {
        char* occ_end = reinterpret_cast<char*>(prev_occupied) + block_size(to_block(prev_occupied));
        if (occ_end == current_start)
        {
            _occupied_ptr = prev_occupied;
            _occupied = true;
        }
        else
        {
            _occupied_ptr = occ_end;
            _occupied = false;
        }
    }
    else
    {
        _occupied_ptr = area_start;
        _occupied = false;
    }

    return *this;
}

allocator_boundary_tags::boundary_iterator
allocator_boundary_tags::boundary_iterator::operator++(
    int n)
{
    auto tmp = *this;
    ++(*this);
    return tmp;
}

allocator_boundary_tags::boundary_iterator
allocator_boundary_tags::boundary_iterator::operator--(
    int n)
{
    auto tmp = *this;
    --(*this);
    return tmp;
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept
{
    if (!_trusted_memory || !_occupied_ptr)
        return 0;

    if (_occupied)
        return block_size(to_block(_occupied_ptr));
    else
    {
        auto& header = get_header(_trusted_memory);
        auto* next_occ = static_cast<block_header*>(header.first_occupied);
        while (next_occ && reinterpret_cast<char*>(next_occ) <= _occupied_ptr)
            next_occ = static_cast<block_header*>(next_occ->next);

        char* end = next_occ ? reinterpret_cast<char*>(next_occ) : block_area_end(_trusted_memory);
        return end - reinterpret_cast<char*>(_occupied_ptr);
    }
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied;
}

void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    if (!_occupied_ptr)
        return nullptr;
    return _occupied
        ? reinterpret_cast<char*>(_occupied_ptr) + OCCUPIED_BLOCK_META_SIZE
        : _occupied_ptr;
}

void* allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    return _trusted_memory
        ? boundary_iterator(const_cast<void*>(_trusted_memory))
        : boundary_iterator();
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    return boundary_iterator();
}