"""8-bit PNG reader and writer.

The metric tools pin numpy, OpenEXR and flip-evaluator. Masks and LDR frames
are PNG, so this module decodes the non-interlaced 8-bit gray, RGB and RGBA
files Godot writes, including the four standard row filters.
"""

from __future__ import annotations

import struct
import zlib

import numpy as np
from numpy.typing import NDArray

_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def encode_png(image: NDArray[np.uint8]) -> bytes:
    array = np.asarray(image, dtype=np.uint8)
    if array.ndim == 2:
        color_type = 0
        packed = array
    elif array.ndim == 3 and array.shape[2] in (3, 4):
        color_type = 2 if array.shape[2] == 3 else 6
        int(array.shape[2])
        packed = array
    else:
        raise ValueError("PNG writer expects HxW, HxWx3 or HxWx4 uint8")
    height, width = int(packed.shape[0]), int(packed.shape[1])
    raw = bytearray()
    for row in range(height):
        raw.append(0)
        raw.extend(packed[row].reshape(-1).tobytes())
    ihdr = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    return b"".join((
        _SIGNATURE,
        _chunk(b"IHDR", ihdr),
        _chunk(b"IDAT", zlib.compress(bytes(raw), 9)),
        _chunk(b"IEND", b""),
    ))


def decode_png(data: bytes) -> NDArray[np.uint8]:
    if not data.startswith(_SIGNATURE):
        raise ValueError("not a PNG")
    offset = len(_SIGNATURE)
    width = height = color_type = -1
    idat = bytearray()
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError("truncated PNG")
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        kind = data[offset + 4 : offset + 8]
        start = offset + 8
        end = start + length
        chunk = data[start:end]
        offset = end + 4
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _comp, _filter, interlace = struct.unpack(">IIBBBBB", chunk)
            if bit_depth != 8 or interlace != 0 or color_type not in (0, 2, 6):
                raise ValueError("only non-interlaced 8-bit gray, RGB or RGBA PNG is supported")
        elif kind == b"IDAT":
            idat.extend(chunk)
        elif kind == b"IEND":
            break
    if width < 0 or not idat:
        raise ValueError("PNG is missing IHDR or IDAT")
    channels = {0: 1, 2: 3, 6: 4}[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rows = np.empty((height, width, channels), dtype=np.uint8)
    previous = np.zeros(stride, dtype=np.uint8)
    cursor = 0
    for y in range(height):
        if cursor + 1 + stride > len(raw):
            raise ValueError("PNG rows are short")
        filter_type = raw[cursor]
        cursor += 1
        current = np.frombuffer(raw[cursor : cursor + stride], dtype=np.uint8).copy()
        cursor += stride
        _unfilter(filter_type, current, previous, channels)
        rows[y] = current.reshape(width, channels)
        previous = current
    if channels == 1:
        return rows.reshape(height, width)
    return rows


def _chunk(kind: bytes, payload: bytes) -> bytes:
    check = zlib.crc32(kind)
    check = zlib.crc32(payload, check) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", check)


def _unfilter(filter_type: int, current: NDArray[np.uint8], previous: NDArray[np.uint8], channels: int) -> None:
    if filter_type == 0:
        return
    if filter_type == 1:
        for index in range(channels, current.size):
            current[index] = (int(current[index]) + int(current[index - channels])) & 0xFF
        return
    if filter_type == 2:
        current[:] = (current.astype(np.uint16) + previous.astype(np.uint16)) & 0xFF
        return
    if filter_type == 3:
        for index in range(current.size):
            left = int(current[index - channels]) if index >= channels else 0
            current[index] = (int(current[index]) + ((left + int(previous[index])) // 2)) & 0xFF
        return
    if filter_type == 4:
        for index in range(current.size):
            left = int(current[index - channels]) if index >= channels else 0
            up = int(previous[index])
            up_left = int(previous[index - channels]) if index >= channels else 0
            current[index] = (int(current[index]) + _paeth(left, up, up_left)) & 0xFF
        return
    raise ValueError(f"unsupported PNG filter {filter_type}")


def _paeth(left: int, up: int, up_left: int) -> int:
    estimate = left + up - up_left
    distance_left = abs(estimate - left)
    distance_up = abs(estimate - up)
    distance_up_left = abs(estimate - up_left)
    if distance_left <= distance_up and distance_left <= distance_up_left:
        return left
    if distance_up <= distance_up_left:
        return up
    return up_left
