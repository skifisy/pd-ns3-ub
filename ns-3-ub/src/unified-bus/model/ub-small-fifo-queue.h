// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_SMALL_FIFO_QUEUE_H
#define UB_SMALL_FIFO_QUEUE_H

#include "ns3/assert.h"

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace ns3 {

struct UbSmallFifoQueueTestAccess;

// FIFO for queues that are usually empty or tiny. Inline slots avoid heap
// allocation; bursts use a vector ring that is released after the queue drains.
template <typename T, std::size_t InlineN = 4>
class UbSmallFifoQueue
{
    static_assert(InlineN > 0, "UbSmallFifoQueue requires at least one inline slot");
    static_assert(std::is_default_constructible_v<T>,
                  "UbSmallFifoQueue requires default-constructible elements");
    static_assert(std::is_move_assignable_v<T>,
                  "UbSmallFifoQueue requires move-assignable elements");

  public:
    bool empty() const
    {
        return m_size == 0;
    }

    std::size_t size() const
    {
        return m_size;
    }

    T& front()
    {
        NS_ASSERT(!empty());
        return ActiveStorage()[m_head];
    }

    const T& front() const
    {
        NS_ASSERT(!empty());
        return ActiveStorage()[m_head];
    }

    void push(const T& value)
    {
        PushImpl(value);
    }

    void push(T&& value)
    {
        PushImpl(std::move(value));
    }

    void pop()
    {
        NS_ASSERT(!empty());
        ActiveStorage()[m_head] = T();
        const std::size_t capacity = ActiveCapacity();
        --m_size;
        if (m_size == 0)
        {
            ReleaseHeapIfNeeded();
            m_head = 0;
            return;
        }
        m_head = (m_head + 1) % capacity;
    }

  private:
    friend struct UbSmallFifoQueueTestAccess;

    template <typename U>
    void PushImpl(U&& value)
    {
        EnsureWritableSlot();
        ActiveStorage()[PhysicalIndex(m_size)] = std::forward<U>(value);
        ++m_size;
    }

    std::size_t ActiveCapacity() const
    {
        return m_usingHeap ? m_heap.size() : InlineN;
    }

    std::size_t PhysicalIndex(std::size_t logicalOffset) const
    {
        return (m_head + logicalOffset) % ActiveCapacity();
    }

    T* ActiveStorage()
    {
        return m_usingHeap ? m_heap.data() : m_inline.data();
    }

    const T* ActiveStorage() const
    {
        return m_usingHeap ? m_heap.data() : m_inline.data();
    }

    void EnsureWritableSlot()
    {
        if (!m_usingHeap && m_size < InlineN)
        {
            return;
        }
        if (!m_usingHeap)
        {
            PromoteToHeap(InlineN * 2);
            return;
        }
        if (m_size < m_heap.size())
        {
            return;
        }
        ResizeHeap(m_heap.size() * 2);
    }

    void PromoteToHeap(std::size_t newCapacity)
    {
        std::vector<T> newBuffer(newCapacity);
        for (std::size_t i = 0; i < m_size; ++i)
        {
            const std::size_t index = PhysicalIndex(i);
            newBuffer[i] = std::move(m_inline[index]);
            m_inline[index] = T();
        }
        m_heap.swap(newBuffer);
        m_usingHeap = true;
        m_head = 0;
    }

    void ResizeHeap(std::size_t newCapacity)
    {
        std::vector<T> newBuffer(newCapacity);
        for (std::size_t i = 0; i < m_size; ++i)
        {
            newBuffer[i] = std::move(m_heap[PhysicalIndex(i)]);
        }
        m_heap.swap(newBuffer);
        m_head = 0;
    }

    void ReleaseHeapIfNeeded()
    {
        if (!m_usingHeap)
        {
            return;
        }
        std::vector<T>().swap(m_heap);
        m_usingHeap = false;
    }

    std::array<T, InlineN> m_inline;
    std::vector<T> m_heap;
    bool m_usingHeap{false};
    std::size_t m_head{0};
    std::size_t m_size{0};
};

struct UbSmallFifoQueueTestAccess
{
    template <typename T, std::size_t InlineN>
    static bool IsUsingHeap(const UbSmallFifoQueue<T, InlineN>& queue)
    {
        return queue.m_usingHeap;
    }

    template <typename T, std::size_t InlineN>
    static std::size_t HeapCapacity(const UbSmallFifoQueue<T, InlineN>& queue)
    {
        return queue.m_heap.capacity();
    }
};

} // namespace ns3

#endif /* UB_SMALL_FIFO_QUEUE_H */
