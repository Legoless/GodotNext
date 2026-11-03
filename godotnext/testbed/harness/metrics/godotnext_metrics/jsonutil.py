"""Narrow JSON values. Bad shapes raise ValueError so the CLI can report them."""

from __future__ import annotations


def as_float(value: object) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"expected a number, got {type(value).__name__}")
    return float(value)


def as_int(value: object) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"expected an integer, got {type(value).__name__}")
    return int(value)


def as_dict(value: object) -> dict[str, object]:
    if not isinstance(value, dict):
        raise ValueError(f"expected an object, got {type(value).__name__}")
    return {str(key): item for key, item in value.items()}


def as_list(value: object) -> list[object]:
    if not isinstance(value, list):
        raise ValueError(f"expected a list, got {type(value).__name__}")
    return list(value)
