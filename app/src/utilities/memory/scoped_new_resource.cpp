#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/linker/linker-defs.h>

#include "scoped_new_resource.h"

namespace eerie_leap::utilities::memory {

namespace {

std::atomic<k_tid_t> scope_owner { nullptr };
// Only the owning thread touches it.
std::pmr::memory_resource* scope_resource = nullptr;

} // namespace

ScopedNewResource::ScopedNewResource(std::pmr::memory_resource* resource) noexcept {
    const k_tid_t self = k_current_get();
    k_tid_t owner = nullptr;
    is_outermost_ = scope_owner.compare_exchange_strong(owner, self);
    is_owner_ = is_outermost_ || owner == self;
    if(is_owner_)
        previous_ = std::exchange(scope_resource, resource);
}

ScopedNewResource::~ScopedNewResource() {
    if(!is_owner_)
        return;

    scope_resource = previous_;
    if(is_outermost_)
        scope_owner.store(nullptr);
}

} // namespace eerie_leap::utilities::memory

// Zephyr places the libc arena between these on Espressif SoCs, so `delete` can tell its blocks apart.
#if defined(CONFIG_COMMON_LIBC_MALLOC) && defined(CONFIG_HAS_ESPRESSIF_HAL) \
    && CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE < 0

extern char _heap_sentry[];

namespace eerie_leap::utilities::memory {

namespace {

struct BlockHeader {
    std::pmr::memory_resource* resource;
    std::size_t bytes;
};

constexpr std::size_t block_alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
constexpr std::size_t header_bytes = (sizeof(BlockHeader) + block_alignment - 1) / block_alignment * block_alignment;

// Owner-only, like scope_resource.
bool in_scope_resource = false;

bool IsLibcBlock(const void* pointer) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    return address >= reinterpret_cast<uintptr_t>(_end) && address < reinterpret_cast<uintptr_t>(_heap_sentry);
}

std::pmr::memory_resource* CurrentResource() {
    const k_tid_t owner = scope_owner.load(std::memory_order_relaxed);
    return owner != nullptr && owner == k_current_get() && !in_scope_resource ? scope_resource : nullptr;
}

// Null when the resource handed back libc memory, which `delete` could not route back to it.
void* AllocateFromResource(std::pmr::memory_resource* resource, std::size_t size) {
    // A resource that itself uses plain `new` must reach libc rather than recurse.
    in_scope_resource = true;
    struct Restore { ~Restore() { in_scope_resource = false; } } restore;

    const std::size_t bytes = header_bytes + size;
    auto* block = static_cast<std::byte*>(resource->allocate(bytes, block_alignment));
    if(IsLibcBlock(block)) {
        resource->deallocate(block, bytes, block_alignment);
        return nullptr;
    }

    *reinterpret_cast<BlockHeader*>(block) = { resource, bytes };
    return block + header_bytes;
}

} // namespace

} // namespace eerie_leap::utilities::memory

// libstdc++ sends the array, sized and nothrow forms through these two; aligned forms stay on libc.
void* operator new(std::size_t size) {
    using namespace eerie_leap::utilities::memory;

    void* pointer = nullptr;
    if(auto* resource = CurrentResource())
        pointer = AllocateFromResource(resource, size);
    if(pointer == nullptr)
        pointer = std::malloc(size == 0 ? 1 : size);
    if(pointer == nullptr)
        throw std::bad_alloc();

    return pointer;
}

void operator delete(void* pointer) noexcept {
    using namespace eerie_leap::utilities::memory;

    if(pointer == nullptr || IsLibcBlock(pointer)) {
        std::free(pointer);
        return;
    }

    auto* block = static_cast<std::byte*>(pointer) - header_bytes;
    const auto header = *reinterpret_cast<const BlockHeader*>(block);
    header.resource->deallocate(block, header.bytes, block_alignment);
}

#endif
