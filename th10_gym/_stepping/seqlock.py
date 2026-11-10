"""Sequence locks for records shared with the hook DLL.

A writer makes the counter odd, writes the record, then makes it even. A reader
accepts a copy only when the counter was even and unchanged around the copy.
"""

import ctypes
from contextlib import contextmanager


def stable_read(read_counter, read_bytes, counter_offset, data_offset, size, attempts=100):
    """Return (counter, bytes) once two consecutive copies agree, or None."""
    previous = None
    for _ in range(attempts):
        before = read_counter(counter_offset)
        if not before or before & 1:
            continue
        body = read_bytes(data_offset, size)
        after = read_counter(counter_offset)
        if before != after or after & 1:
            previous = None
            continue
        sample = (after, body)
        if previous == sample:
            return sample
        previous = sample
    return None


def next_even(counter):
    result = (counter + 2) & 0xFFFFFFFF
    return result or 2


def tick_count_ms(channel):
    channel.kernel32.GetTickCount.restype = ctypes.c_uint32
    return channel.kernel32.GetTickCount()


@contextmanager
def publishing(channel, offset):
    """Make a record visible only after the complete payload has been written."""
    current = channel.read_stamp(offset)
    if current & 1:
        raise RuntimeError("A publication is already in progress")
    even = next_even(current)
    channel.atomic_store(offset, current + 1)
    yield even
    channel.atomic_store(offset, even)
