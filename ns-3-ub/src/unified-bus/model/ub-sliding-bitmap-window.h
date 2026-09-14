// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_SLIDING_BITMAP_WINDOW_H
#define UB_SLIDING_BITMAP_WINDOW_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace ns3 {

class UbSlidingBitmapWindow
{
  public:
    explicit UbSlidingBitmapWindow(uint32_t windowSize = 0)
    {
        Resize(windowSize);
    }

    void Resize(uint32_t windowSize)
    {
        m_windowSize = windowSize;
        m_slots.assign(windowSize, kInvalidSequence);
    }

    void Reset(uint64_t baseSequence = 0)
    {
        m_baseSequence = baseSequence;
        std::fill(m_slots.begin(), m_slots.end(), kInvalidSequence);
    }

    [[nodiscard]] uint32_t GetWindowSize() const
    {
        return m_windowSize;
    }

    [[nodiscard]] uint64_t GetBase() const
    {
        return m_baseSequence;
    }

    [[nodiscard]] bool Mark(uint64_t sequence)
    {
        if (!IsInWindow(sequence) || m_windowSize == 0) {
            return false;
        }
        m_slots[GetSlotIndex(sequence)] = sequence;
        return true;
    }

    [[nodiscard]] bool Contains(uint64_t sequence) const
    {
        if (!IsInWindow(sequence) || m_windowSize == 0) {
            return false;
        }
        return m_slots[GetSlotIndex(sequence)] == sequence;
    }

    uint32_t AdvanceContiguous()
    {
        if (m_windowSize == 0) {
            return 0;
        }

        uint32_t advanced = 0;
        while (Contains(m_baseSequence)) {
            m_slots[GetSlotIndex(m_baseSequence)] = kInvalidSequence;
            ++m_baseSequence;
            ++advanced;
        }
        return advanced;
    }

  private:
    static constexpr uint64_t kInvalidSequence = std::numeric_limits<uint64_t>::max();

    [[nodiscard]] bool IsInWindow(uint64_t sequence) const
    {
        return sequence >= m_baseSequence && sequence < m_baseSequence + m_windowSize;
    }

    [[nodiscard]] uint32_t GetSlotIndex(uint64_t sequence) const
    {
        return static_cast<uint32_t>(sequence % m_windowSize);
    }

    uint64_t m_baseSequence{0};
    uint32_t m_windowSize{0};
    std::vector<uint64_t> m_slots;
};

} // namespace ns3

#endif /* UB_SLIDING_BITMAP_WINDOW_H */
