#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <new>
#include <utility>
#include <unordered_map>
#include <type_traits>

#include <mutex>
#include <atomic>

namespace helios {

// Fast, thread-safe, chunked object slab pool for connection state.
// Eliminates dynamic heap allocation on connection accept/close while keeping pointers stable.
template <typename T, size_t ChunkSize = 512>
class ConnectionSlabPool {
public:
    explicit ConnectionSlabPool(size_t initial_chunks = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < (initial_chunks == 0 ? 1 : initial_chunks); ++i) {
            AllocateChunk();
        }
    }

    ~ConnectionSlabPool() {
        Clear();
    }

    // Non-copyable, non-movable
    ConnectionSlabPool(const ConnectionSlabPool&) = delete;
    ConnectionSlabPool& operator=(const ConnectionSlabPool&) = delete;
    ConnectionSlabPool(ConnectionSlabPool&&) = delete;
    ConnectionSlabPool& operator=(ConnectionSlabPool&&) = delete;

    // Acquire a new object constructed in-place
    template <typename... Args>
    T* Acquire(Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_indices_.empty()) {
            AllocateChunk();
        }

        uint32_t idx = free_indices_.back();
        free_indices_.pop_back();

        size_t chunk_idx = idx / ChunkSize;
        size_t slot_idx = idx % ChunkSize;
        void* slot = static_cast<void*>(&chunks_[chunk_idx]->slots[slot_idx]);

        T* obj = ::new (slot) T(std::forward<Args>(args)...);
        ptr_to_index_[obj] = idx;
        active_count_.fetch_add(1, std::memory_order_relaxed);
        return obj;
    }

    // Release an object, calling its destructor and recycling its slot
    void Release(T* ptr) {
        if (!ptr) return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = ptr_to_index_.find(ptr);
        if (it == ptr_to_end_()) {
            return;
        }

        uint32_t idx = it->second;
        ptr_to_index_.erase(it);

        ptr->~T();
        free_indices_.push_back(idx);
        if (active_count_.load(std::memory_order_relaxed) > 0) {
            active_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] size_t ActiveCount() const noexcept {
        return active_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t TotalCapacity() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return chunks_.size() * ChunkSize;
    }

    [[nodiscard]] size_t FreeCount() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return free_indices_.size();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        // Destruct any still-active objects
        for (const auto& [ptr, idx] : ptr_to_index_) {
            ptr->~T();
        }
        ptr_to_index_.clear();
        free_indices_.clear();
        chunks_.clear();
        active_count_.store(0, std::memory_order_relaxed);
    }

private:
    auto ptr_to_end_() noexcept { return ptr_to_index_.end(); }

    struct alignas(alignof(T)) SlotStorage {
        std::byte data[sizeof(T)];
    };

    struct Chunk {
        SlotStorage slots[ChunkSize];
    };

    void AllocateChunk() {
        uint32_t start_idx = static_cast<uint32_t>(chunks_.size() * ChunkSize);
        auto chunk = std::make_unique<Chunk>();
        chunks_.push_back(std::move(chunk));

        free_indices_.reserve(free_indices_.size() + ChunkSize);
        for (size_t i = 0; i < ChunkSize; ++i) {
            free_indices_.push_back(start_idx + static_cast<uint32_t>(i));
        }
    }

    std::vector<std::unique_ptr<Chunk>> chunks_;
    std::vector<uint32_t> free_indices_;
    std::unordered_map<T*, uint32_t> ptr_to_index_;
    mutable std::mutex mutex_;
    std::atomic<size_t> active_count_{0};
};

} // namespace helios
