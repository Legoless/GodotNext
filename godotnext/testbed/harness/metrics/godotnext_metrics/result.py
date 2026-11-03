"""JSON result shared by every Q metric."""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import TypeAlias

JsonScalar: TypeAlias = str | int | float | bool | None
JsonValue: TypeAlias = JsonScalar | list["JsonValue"] | dict[str, "JsonValue"]


def json_ready(value: object) -> JsonValue:
    """Convert a result into strict JSON. Non-finite floats become null."""
    if value is None or isinstance(value, (str, bool, int)):
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            return None
        return value
    if isinstance(value, dict):
        return {str(key): json_ready(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [json_ready(item) for item in value]
    # numpy scalars
    if hasattr(value, "item") and callable(value.item):
        return json_ready(value.item())
    raise TypeError(f"cannot encode {type(value).__name__} as JSON")


@dataclass
class MetricResult:
    """One metric evaluation. `pass` is the gate bit emitted as JSON."""

    metric: str
    value: object
    threshold: object
    passed: bool
    values: dict[str, object] = field(default_factory=dict)
    details: dict[str, object] = field(default_factory=dict)

    def to_json(self) -> dict[str, JsonValue]:
        payload: dict[str, JsonValue] = {
            "metric": self.metric,
            "pass": self.passed,
            "threshold": json_ready(self.threshold),
            "value": json_ready(self.value),
        }
        if self.values:
            payload["values"] = json_ready(self.values)
        if self.details:
            payload["details"] = json_ready(self.details)
        return payload


class MetricInputError(Exception):
    """The capture directory does not contain what this metric reads."""

    def __init__(self, metric: str, message: str) -> None:
        super().__init__(message)
        self.metric = metric
        self.message = message

    def to_result(self) -> MetricResult:
        return MetricResult(
            metric=self.metric,
            value=None,
            threshold=None,
            passed=False,
            details={"error": self.message},
        )
