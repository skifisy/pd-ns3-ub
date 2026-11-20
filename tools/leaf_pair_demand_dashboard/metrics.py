"""Shared units and statistics. Rates always use actual recorded durations."""
from decimal import Decimal, InvalidOperation
import math

PS_PER_SECOND = 10**12


def picoseconds(ns):
    try:
        value = Decimal(str(ns)) * 1000
    except InvalidOperation as error:
        raise ValueError(f"Invalid nanosecond timestamp: {ns}") from error
    if not value.is_finite() or value < 0 or value != value.to_integral_value():
        raise ValueError(f"Invalid nanosecond timestamp: {ns}")
    return int(value)


def finite_number(value, name, positive=False):
    number = float(value)
    if not math.isfinite(number) or number < 0 or (positive and number == 0):
        raise ValueError(f"{name} must be {'positive' if positive else 'nonnegative'} and finite")
    return number


def rate(byte_count, duration_ps):
    return byte_count * 8 * PS_PER_SECOND / duration_ps


def weighted_quantiles(values, total_duration):
    """Time-weighted nearest-rank quantiles, including idle windows."""
    result = {}
    elapsed = 0
    targets = iter((("p95", .95), ("p99", .99)))
    target = next(targets, None)
    for value, duration in sorted(values):
        elapsed += duration
        while target and elapsed >= total_duration * target[1]:
            result[target[0]] = value
            target = next(targets, None)
        if target is None:
            break
    return result
