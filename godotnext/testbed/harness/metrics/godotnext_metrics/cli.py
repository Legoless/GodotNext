"""Command line for one §13.4 metric.

Exit 0 when a result was computed, including a failed gate. Exit 2 when the
metric name is unknown or the capture is missing inputs. Both paths print JSON.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from godotnext_metrics.commands import run_metric
from godotnext_metrics.result import MetricInputError, MetricResult


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m godotnext_metrics")
    parser.add_argument("metric", help="q1, q4a, q4b, … q21")
    parser.add_argument("--in", dest="in_dir", required=True, help="capture directory")
    parser.add_argument("--ref", dest="ref_dir", help="reference capture directory")
    parser.add_argument("--json", action="store_true", help="print the JSON result (this is also the default)")
    args = parser.parse_args(argv)
    _ = args.json
    reference = Path(args.ref_dir) if args.ref_dir else None
    try:
        result = run_metric(args.metric, Path(args.in_dir), reference)
    except MetricInputError as exc:
        _emit(exc.to_result())
        return 2
    except (OSError, ValueError, KeyError) as exc:
        _emit(MetricInputError(args.metric, str(exc)).to_result())
        return 2
    _emit(result)
    return 0


def _emit(result: MetricResult) -> None:
    json.dump(result.to_json(), sys.stdout, sort_keys=True, allow_nan=False)
    sys.stdout.write("\n")
