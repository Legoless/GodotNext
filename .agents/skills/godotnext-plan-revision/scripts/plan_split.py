#!/usr/bin/env python3
"""Split the GodotNext plan into one fragment per top-level <section>, so agents can edit sections in parallel.

    python3 plan_split.py docs/godotnext/nextgi-nextgeo-plan.html OUT_DIR

Writes OUT_DIR/NN-<section id>.html, plus 00-head.html (everything before the first section) and
99-tail.html (everything after the last), and OUT_DIR/order.json. plan_join.py reverses it byte for byte.
"""

import json
import os
import re
import sys

src, out = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
os.makedirs(out, exist_ok=True)
starts = [(m.start(), m.group(1)) for m in re.finditer(r'^<section id="([^"]+)">', text, re.M)]
if not starts:
    sys.exit("no top-level <section id=...> found")
end = text.rfind("</section>") + len("</section>")
if text[end : end + 1] == "\n":
    end += 1
cuts = [(0, "00-head")] + [(pos, f"{i + 1:02d}-{sid}") for i, (pos, sid) in enumerate(starts)] + [(end, "99-tail")]
order = []
for (a, name), (b, _) in zip(cuts, cuts[1:] + [(len(text), "")]):
    open(os.path.join(out, name + ".html"), "w", encoding="utf-8").write(text[a:b])
    order.append(name)
json.dump(order, open(os.path.join(out, "order.json"), "w"), indent=1)
print(f"{len(order)} fragments in {out}")
