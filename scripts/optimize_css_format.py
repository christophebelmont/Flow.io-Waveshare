#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import List, Tuple


@dataclass
class StyleRule:
    selectors: List[str]
    declarations: str


@dataclass
class AtRuleBlock:
    header: str
    items: List[object]


@dataclass
class AtRuleStmt:
    stmt: str


@dataclass
class RawChunk:
    text: str


def strip_comments(css: str) -> str:
    out = []
    i = 0
    n = len(css)
    in_str = None
    while i < n:
        ch = css[i]
        if in_str:
            out.append(ch)
            if ch == '\\' and i + 1 < n:
                out.append(css[i + 1])
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch in ('"', "'"):
            in_str = ch
            out.append(ch)
            i += 1
            continue
        if ch == '/' and i + 1 < n and css[i + 1] == '*':
            i += 2
            while i + 1 < n and not (css[i] == '*' and css[i + 1] == '/'):
                i += 1
            i += 2 if i + 1 <= n else 1
            continue
        out.append(ch)
        i += 1
    return ''.join(out)


def read_until(css: str, i: int, stop_chars: str) -> Tuple[str, int, str]:
    n = len(css)
    out = []
    in_str = None
    paren = 0
    bracket = 0
    while i < n:
        ch = css[i]
        if in_str:
            out.append(ch)
            if ch == '\\' and i + 1 < n:
                out.append(css[i + 1])
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch in ('"', "'"):
            in_str = ch
            out.append(ch)
            i += 1
            continue
        if ch == '(':
            paren += 1
            out.append(ch)
            i += 1
            continue
        if ch == ')':
            paren = max(0, paren - 1)
            out.append(ch)
            i += 1
            continue
        if ch == '[':
            bracket += 1
            out.append(ch)
            i += 1
            continue
        if ch == ']':
            bracket = max(0, bracket - 1)
            out.append(ch)
            i += 1
            continue
        if paren == 0 and bracket == 0 and ch in stop_chars:
            return ''.join(out), i, ch
        out.append(ch)
        i += 1
    return ''.join(out), i, ''


def read_block(css: str, i: int) -> Tuple[str, int]:
    assert i < len(css) and css[i] == '{'
    i += 1
    depth = 1
    out = []
    in_str = None
    n = len(css)
    while i < n and depth > 0:
        ch = css[i]
        if in_str:
            out.append(ch)
            if ch == '\\' and i + 1 < n:
                out.append(css[i + 1])
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch in ('"', "'"):
            in_str = ch
            out.append(ch)
            i += 1
            continue
        if ch == '{':
            depth += 1
            out.append(ch)
            i += 1
            continue
        if ch == '}':
            depth -= 1
            if depth == 0:
                i += 1
                break
            out.append(ch)
            i += 1
            continue
        out.append(ch)
        i += 1
    return ''.join(out), i


def normalize_whitespace_outside_strings(text: str) -> str:
    out = []
    in_str = None
    prev_space = False
    for ch in text:
        if in_str:
            out.append(ch)
            if ch == '\\':
                prev_space = False
                continue
            if ch == in_str:
                in_str = None
            continue
        if ch in ('"', "'"):
            in_str = ch
            out.append(ch)
            prev_space = False
            continue
        if ch.isspace():
            if not prev_space:
                out.append(' ')
            prev_space = True
            continue
        out.append(ch)
        prev_space = False
    text = ''.join(out).strip()
    # Trim spaces around punctuation tokens where safe.
    for token in [':', ';', ',', '{', '}', '>', '+', '~', '=']:
        text = text.replace(' ' + token, token).replace(token + ' ', token)
    # Keep space before combinators for selector readability/validity where needed.
    text = text.replace('>',' > ').replace('+',' + ').replace('~',' ~ ')
    text = ' '.join(text.split())
    text = text.replace(' > ', '>').replace(' + ', '+').replace(' ~ ', '~')
    return text


def parse_declarations(body: str) -> List[str]:
    n = len(body)
    i = 0
    chunk = []
    out: List[str] = []
    in_str = None
    paren = 0
    while i < n:
        ch = body[i]
        if in_str:
            chunk.append(ch)
            if ch == '\\' and i + 1 < n:
                chunk.append(body[i + 1])
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch in ('"', "'"):
            in_str = ch
            chunk.append(ch)
            i += 1
            continue
        if ch == '(':
            paren += 1
            chunk.append(ch)
            i += 1
            continue
        if ch == ')':
            paren = max(0, paren - 1)
            chunk.append(ch)
            i += 1
            continue
        if ch == ';' and paren == 0:
            raw = ''.join(chunk).strip()
            if raw:
                out.append(raw)
            chunk = []
            i += 1
            continue
        chunk.append(ch)
        i += 1
    tail = ''.join(chunk).strip()
    if tail:
        out.append(tail)
    return out


def normalize_declarations(body: str) -> str:
    decls = parse_declarations(body)
    seen = set()
    normalized: List[str] = []
    for decl in decls:
        clean = normalize_whitespace_outside_strings(decl)
        if not clean:
            continue
        # Remove exact duplicate declaration tokens only.
        if clean in seen:
            continue
        seen.add(clean)
        normalized.append(clean)
    return ';'.join(normalized)


def parse_items(css: str) -> List[object]:
    n = len(css)
    i = 0
    items: List[object] = []
    while i < n:
        while i < n and css[i].isspace():
            i += 1
        if i >= n:
            break
        if css[i] == '@':
            head, j, stop = read_until(css, i, '{;')
            head = normalize_whitespace_outside_strings(head)
            if stop == ';':
                items.append(AtRuleStmt(head + ';'))
                i = j + 1
                continue
            if stop == '{':
                inner, k = read_block(css, j)
                items.append(AtRuleBlock(head, parse_items(inner)))
                i = k
                continue
            items.append(RawChunk(css[i:]))
            break
        selector, j, stop = read_until(css, i, '{')
        selector = selector.strip()
        if stop != '{':
            if selector:
                items.append(RawChunk(selector))
            break
        body, k = read_block(css, j)
        selectors = [s.strip() for s in selector.split(',') if s.strip()]
        if selectors:
            items.append(StyleRule(selectors, normalize_declarations(body)))
        i = k
    return items


def optimize_items(items: List[object]) -> List[object]:
    optimized: List[object] = []
    # First recursively optimize nested blocks.
    for item in items:
        if isinstance(item, AtRuleBlock):
            optimized.append(AtRuleBlock(item.header, optimize_items(item.items)))
        else:
            optimized.append(item)

    # Then merge style rules with identical declaration blocks in the same scope.
    decl_to_index = {}
    selector_seen = {}
    out: List[object] = []
    for item in optimized:
        if not isinstance(item, StyleRule):
            out.append(item)
            continue
        decl = item.declarations
        if not decl:
            continue
        if decl not in decl_to_index:
            decl_to_index[decl] = len(out)
            selector_seen[decl] = set(item.selectors)
            out.append(StyleRule(list(item.selectors), decl))
            continue
        idx = decl_to_index[decl]
        base = out[idx]
        if not isinstance(base, StyleRule):
            out.append(item)
            continue
        seen = selector_seen[decl]
        for sel in item.selectors:
            if sel not in seen:
                base.selectors.append(sel)
                seen.add(sel)
    return out


def render_items(items: List[object], indent: int = 0) -> str:
    pad = '  ' * indent
    lines: List[str] = []
    for item in items:
        if isinstance(item, StyleRule):
            sel = ','.join(item.selectors)
            lines.append(f"{pad}{sel}{{{item.declarations};}}")
        elif isinstance(item, AtRuleStmt):
            lines.append(f"{pad}{item.stmt}")
        elif isinstance(item, AtRuleBlock):
            lines.append(f"{pad}{item.header}{{")
            lines.append(render_items(item.items, indent + 1))
            lines.append(f"{pad}}}")
        elif isinstance(item, RawChunk):
            text = item.text.strip()
            if text:
                lines.append(f"{pad}{text}")
    return '\n'.join([line for line in lines if line is not None and line != ''])


def optimize_css_text(css: str) -> str:
    css = strip_comments(css)
    items = parse_items(css)
    items = optimize_items(items)
    out = render_items(items)
    return out.strip() + '\n'


def main() -> int:
    project = Path(__file__).resolve().parents[1]
    src = project / 'data' / 'webinterface' / 'app-core.css'
    dst_primary = project / 'data' / 'webinterface' / 'app-core.css'

    original = src.read_text(encoding='utf-8', errors='ignore')
    optimized = optimize_css_text(original)

    dst_primary.write_text(optimized, encoding='utf-8')

    print(f"[optimize_css] before={len(original)} after={len(optimized)} saved={len(original)-len(optimized)}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
