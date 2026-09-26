#pragma once

#include <memory_resource>

namespace eerie_leap::utilities::memory {

// While alive, plain `new` on the constructing thread allocates from `resource` instead of the
// libc arena, and `delete` returns such blocks to their resource from any thread. One thread
// holds scopes at a time: nested scopes on it stack, a scope opened elsewhere meanwhile has no
// effect, and a null resource suspends the redirect. The resource must be thread-safe and outlive
// its blocks; one backed by the libc arena has no effect. Where the libc arena's bounds are not
// known (anything but Espressif SoCs) `new` is unaffected.
class ScopedNewResource {
private:
    std::pmr::memory_resource* previous_ = nullptr;
    bool is_owner_ = false;
    bool is_outermost_ = false;

public:
    explicit ScopedNewResource(std::pmr::memory_resource* resource) noexcept;
    ~ScopedNewResource();

    ScopedNewResource(const ScopedNewResource&) = delete;
    ScopedNewResource& operator=(const ScopedNewResource&) = delete;
};

} // namespace eerie_leap::utilities::memory
