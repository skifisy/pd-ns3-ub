// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_MODULO_SEQUENCE_H
#define UB_MODULO_SEQUENCE_H

#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace ns3 {

template <uint8_t WireBits, typename LogicalT = uint64_t>
class UbModuloSequence
{
    static_assert(std::is_unsigned_v<LogicalT>, "Logical sequence type must be unsigned");
    static_assert(WireBits > 0, "WireBits must be positive");
    static_assert(WireBits < std::numeric_limits<LogicalT>::digits,
                  "Logical type must be wider than the wire sequence space");
    static_assert(WireBits <= 32, "Wire sequence values are exposed as uint32_t");

public:
    static constexpr LogicalT Modulus = LogicalT{1} << WireBits;
    static constexpr LogicalT Mask = Modulus - 1;
    static constexpr LogicalT HalfRange = Modulus / 2;

    static uint32_t ToWire(LogicalT logical)
    {
        return static_cast<uint32_t>(logical & Mask);
    }

    static LogicalT Unwrap(uint32_t wire, LogicalT reference)
    {
        const LogicalT wireValue = static_cast<LogicalT>(wire) & Mask;
        const LogicalT base = reference & ~Mask;
        LogicalT candidate = base | wireValue;

        if (candidate + HalfRange < reference)
        {
            candidate += Modulus;
        }
        else if (candidate > reference + HalfRange && candidate >= Modulus)
        {
            candidate -= Modulus;
        }
        return candidate;
    }

    static LogicalT UnwrapAtOrAfter(uint32_t wire, LogicalT lowerBound)
    {
        LogicalT candidate = Unwrap(wire, lowerBound);
        if (candidate < lowerBound)
        {
            candidate += Modulus;
        }
        return candidate;
    }

    static std::optional<LogicalT> UnwrapInWindow(uint32_t wire, LogicalT base, LogicalT limit)
    {
        if (limit <= base)
        {
            return std::nullopt;
        }

        const LogicalT candidate = Unwrap(wire, base);
        if (candidate >= base && candidate < limit)
        {
            return candidate;
        }
        return std::nullopt;
    }
};

} // namespace ns3

#endif // UB_MODULO_SEQUENCE_H
