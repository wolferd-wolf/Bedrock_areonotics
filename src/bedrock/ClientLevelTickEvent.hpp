#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aeronautics::bedrock {

struct ClientLevelTickEvent final {
    void* clientLevel{};
    std::uint64_t sequence{};
    std::uint32_t threadId{};
};

class ClientLevelTickListener {
public:
    virtual ~ClientLevelTickListener() = default;
    virtual void onClientLevelTick(const ClientLevelTickEvent& event) noexcept = 0;
};

class ClientLevelTickBus final {
public:
    static constexpr std::size_t capacity = 8;

    [[nodiscard]] bool subscribe(ClientLevelTickListener& listener) noexcept {
        for (auto& slot : mListeners) {
            ClientLevelTickListener* current = slot.load(std::memory_order_acquire);
            if (current == &listener) {
                return true;
            }
        }

        for (auto& slot : mListeners) {
            ClientLevelTickListener* expected = nullptr;
            if (slot.compare_exchange_strong(
                    expected,
                    &listener,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                mListenerCount.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    void unsubscribe(ClientLevelTickListener& listener) noexcept {
        for (auto& slot : mListeners) {
            ClientLevelTickListener* expected = &listener;
            if (slot.compare_exchange_strong(
                    expected,
                    nullptr,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                mListenerCount.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    [[nodiscard]] std::size_t publish(const ClientLevelTickEvent& event) noexcept {
        std::size_t delivered = 0;
        for (auto& slot : mListeners) {
            if (ClientLevelTickListener* listener =
                    slot.load(std::memory_order_acquire)) {
                listener->onClientLevelTick(event);
                ++delivered;
            }
        }
        mPublishedEvents.fetch_add(1, std::memory_order_relaxed);
        mDeliveredCallbacks.fetch_add(delivered, std::memory_order_relaxed);
        return delivered;
    }

    [[nodiscard]] std::size_t listenerCount() const noexcept {
        return mListenerCount.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t publishedEvents() const noexcept {
        return mPublishedEvents.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t deliveredCallbacks() const noexcept {
        return mDeliveredCallbacks.load(std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<ClientLevelTickListener*>, capacity> mListeners{};
    std::atomic<std::size_t> mListenerCount{0};
    std::atomic<std::uint64_t> mPublishedEvents{0};
    std::atomic<std::uint64_t> mDeliveredCallbacks{0};
};

}  // namespace aeronautics::bedrock
