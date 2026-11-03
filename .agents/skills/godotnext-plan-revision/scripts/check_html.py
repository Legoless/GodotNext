#!/usr/bin/env python3
"""Check the plan's HTML for mismatched or unclosed tags.

    python3 check_html.py docs/godotnext/nextgi-nextgeo-plan.html

Prints "no tag errors" and "unclosed: []" when the file is well formed, and exits 1 otherwise.
"""
import sys
from html.parser import HTMLParser

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}
OPTIONAL = {"p", "li", "td", "th", "tr", "thead", "tbody", "dt", "dd", "option"}


class Checker(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack, self.errors = [], []

    def handle_starttag(self, tag, attrs):
        if tag not in VOID:
            self.stack.append((tag, self.getpos()[0]))

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        while self.stack and self.stack[-1][0] != tag and self.stack[-1][0] in OPTIONAL:
            self.stack.pop()
        if self.stack and self.stack[-1][0] == tag:
            self.stack.pop()
        else:
            self.errors.append(f"line {self.getpos()[0]}: </{tag}> does not match <{self.stack[-1][0] if self.stack else 'nothing'}>")


c = Checker()
c.feed(open(sys.argv[1], encoding="utf-8").read())
c.close()
left = [f"{t}@{line}" for t, line in c.stack if t not in OPTIONAL and t not in ("html", "body", "head")]
print("\n".join(c.errors[:20]) if c.errors else "no tag errors")
print("unclosed:", left[:20])
sys.exit(1 if c.errors or left else 0)
