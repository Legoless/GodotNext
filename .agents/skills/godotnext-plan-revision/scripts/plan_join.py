#!/usr/bin/env python3
"""Join plan fragments (made by plan_split.py) back into one file, in order.json order.

    python3 plan_join.py FRAG_DIR docs/godotnext/nextgi-nextgeo-plan.html

A new section is a new fragment file plus its name in order.json at the right place.
"""

import json
import os
import sys

frag, dst = sys.argv[1], sys.argv[2]
order = json.load(open(os.path.join(frag, "order.json")))
parts = [open(os.path.join(frag, name + ".html"), encoding="utf-8").read() for name in order]
open(dst, "w", encoding="utf-8").write("".join(parts))
print(f"joined {len(order)} fragments into {dst}")
