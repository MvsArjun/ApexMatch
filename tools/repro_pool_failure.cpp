// Standalone standard-library dependency reproducer; no ApexMatch code is used.
// Example: g++ -std=c++20 -O0 -g tools/repro_pool_failure.cpp -o pool_failure_probe
#include <iostream>
#include <array>
#include <memory_resource>
#include <new>

class FailingUpstream final : public std::pmr::memory_resource {
    std::size_t remaining_ = 2;
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        std::cerr << "upstream allocate " << bytes << " alignment " << alignment
                  << " remaining " << remaining_ << '\n';
        if (remaining_ == 0) throw std::bad_alloc();
        --remaining_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* memory, std::size_t bytes, std::size_t alignment) override {
        std::cerr << "upstream deallocate " << bytes << '\n';
        std::pmr::new_delete_resource()->deallocate(memory, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

int main() {
    FailingUpstream upstream;
    try {
        std::pmr::unsynchronized_pool_resource pool(&upstream);
        std::array<void*, 128> blocks{};
        for (auto& memory : blocks) memory = pool.allocate(16, 8);
        for (void* memory : blocks) pool.deallocate(memory, 16, 8);
        std::cerr << "allocation completed without reaching failure point; inconclusive\n";
        return 2;
    } catch (const std::bad_alloc&) {
        std::cerr << "bad_alloc propagated safely\n";
    }
}
