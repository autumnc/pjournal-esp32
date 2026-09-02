#!/usr/bin/env python3
"""Faithful host-side mirror of markdown_render.cpp inline/block parsing.

Works on UTF-8 byte strings (mirrors C++ byte indexing). Verifies:
  - segments are contiguous, non-overlapping, covering [0, len(line))
  - for every segment, width(drawText) == width(raw bytes [start,end))
  - mdClassifyLines classifies blocks correctly (incl. code fence toggle)
"""
import unicodedata

# Mirror g_font.textWidth cell model: ASCII=1 cell, symbol chars=1 cell, CJK=2
SYMBOLS = {"•", "☐", "✓", "■"}
# Heading level markers uF03A4/uF03A7/uF03AA/uF03AD/uF03B1/uF03B3 (levels 1-6)
HL_GLYPHS = {i: chr(cp) for i, cp in
             enumerate((0xF03A4, 0xF03A7, 0xF03AA, 0xF03AD, 0xF03B1, 0xF03B3), start=1)}

def utf8_len(first_byte):
    if first_byte <= 0x7f: return 1
    if first_byte <= 0xdf: return 2
    if first_byte <= 0xef: return 3
    return 4

def cells_of_char(cp):
    if cp in (0x2022, 0x2610, 0x2713, 0x25A0): return 1  # • ☐ ✓ ■
    if cp < 0x80: return 1
    if (0x2e80 <= cp <= 0x9fff) or (0xf900 <= cp <= 0xfaff) or (0xff00 <= cp <= 0xffef):
        return 2
    return 1

def width(line):  # bytes
    i, w = 0, 0
    while i < len(line):
        n = utf8_len(line[i])
        sub = line[i:i+n]
        if len(sub) < n:
            w += 1; i += 1; continue
        try:
            cps = sub.decode('utf-8')
        except UnicodeDecodeError:
            w += 1; i += 1; continue
        for c in cps:
            w += cells_of_char(ord(c))
        i += n
    return w

BS = ord('\\'); ST = ord('*'); BT = ord('`'); LB = ord('['); TL = ord('~'); EQ = ord('=')
SP = ord(' '); HT = ord('#'); DD = ord('-'); PL = ord('+'); GT = ord('>'); US = ord('_')

_CN_NUMS = ('零', '一', '二', '三', '四', '五', '六', '七', '八', '九', '十', '百', '千')

def is_cn_num(line, at):
    """Mirror of isCnNumChar: True if line[at] starts a Chinese numeral char."""
    if at + 3 > len(line):
        return False
    return line[at:at + 3] in (s.encode() for s in _CN_NUMS)

# ---- Mirror of the C++ logic (byte offsets) ----

def spaces_for_width(line, frm, to):
    target = width(line[frm:to])
    sp = b""
    while width(sp) < target:
        sp += b" "
    return sp

def next_marker(line, frm, ln):
    i = frm
    while i < ln:
        c = line[i]
        if c == BS:
            i += 1
        elif c in (ST, BT, LB, TL, EQ):
            return i
        i += 1
    return ln

def find_stars(line, frm, ln, n):
    i = frm
    while i <= ln - n:
        if all(line[i+k] == ST for k in range(n)): return i
        i += 1
    return -1

def emit_plain(line, frm, to, base, segs):
    """Mirror of markdown_render.cpp emitPlain: backslash-escaped ASCII becomes a
    space plus the literal next char."""
    p = frm
    while p < to:
        if line[p] == BS and p + 1 < to and line[p + 1] < 0x80:
            if p > frm:
                segs.append([frm, p, dict(base), line[frm:p]])
            segs.append([p, p + 1, dict(base), b" "])
            segs.append([p + 1, p + 2, dict(base), line[p + 1:p + 2]])
            p += 2
            frm = p
        else:
            p += 1
    if frm < to:
        segs.append([frm, to, dict(base), line[frm:to]])

def md_parse_inline(line, frm, base, segs):
    ln = len(line)
    plain = frm
    while plain < ln:
        m = next_marker(line, plain, ln)
        if m == ln: break
        if m > plain:
            emit_plain(line, plain, m, base, segs)
        c = line[m]
        if c == ST:
            n = 1
            while m + n < ln and line[m+n] == ST: n += 1
            n = min(n, 3)
            cc = find_stars(line, m + n, ln, n)
            if cc < 0:
                segs.append([m, m+1, dict(base), line[m:m+1]]); plain = m + 1; continue
            st = dict(base)
            if n >= 3: st['bold'] = True; st['underline'] = True
            elif n == 2: st['bold'] = True
            else: st['underline'] = True
            segs.append([m, m+n, dict(base), b""])
            if cc > m + n:
                emit_plain(line, m + n, cc, st, segs)
            segs.append([cc, cc+n, dict(base), b""])
            plain = cc + n
            continue
        if c == BT:
            cc = line.find(b'`', m+1)
            if cc < 0 or cc >= ln:
                segs.append([m, m+1, dict(base), line[m:m+1]]); plain = m + 1; continue
            st = dict(base); st['invert'] = True
            segs.append([m, m+1, dict(base), b""])
            emit_plain(line, m + 1, cc, st, segs)
            segs.append([cc, cc+1, dict(base), b""])
            plain = cc + 1
            continue
        if c == TL and m+1 < ln and line[m+1] == TL:
            cc = line.find(b'~~', m+2)
            if cc < 0 or cc >= ln:
                segs.append([m, m+1, dict(base), line[m:m+1]]); plain = m + 1; continue
            st = dict(base); st['strike'] = True
            segs.append([m, m+2, dict(base), b""])
            emit_plain(line, m + 2, cc, st, segs)
            segs.append([cc, cc+2, dict(base), b""])
            plain = cc + 2
            continue
        if c == EQ and m+1 < ln and line[m+1] == EQ:
            cc = line.find(b'==', m+2)
            if cc < 0 or cc >= ln:
                segs.append([m, m+1, dict(base), line[m:m+1]]); plain = m + 1; continue
            st = dict(base); st['emph'] = True
            segs.append([m, m+2, dict(base), b""])
            emit_plain(line, m + 2, cc, st, segs)
            segs.append([cc, cc+2, dict(base), b""])
            plain = cc + 2
            continue
        if c == LB:
            p = line.find(b'](', m+1)
            if p >= 0 and p < ln:
                cp = line.find(b')', p+2)
                if cp >= 0 and cp < ln:
                    st = dict(base); st['invert'] = True; st['underline'] = True
                    segs.append([m, m+1, dict(base), b" "])
                    emit_plain(line, m + 1, p, st, segs)
                    segs.append([p, cp+1, dict(base), spaces_for_width(line, p, cp+1)])
                    plain = cp + 1
                    continue
        segs.append([m, m+1, dict(base), line[m:m+1]]); plain = m + 1
    if plain < ln:
        emit_plain(line, plain, ln, base, segs)

def md_list_marker(line):
    """Mirror of markdown_render.cpp mdListMarker. Returns a dict or None."""
    ln = len(line)
    i = 0
    while i < ln and line[i] in (SP, ord('\t')): i += 1
    rest = ln - i
    if rest >= 5 and line[i:i+5] in (b'- [ ]', b'- [x]', b'- [X]'):
        mlen = 6 if (i + 5 < ln and line[i+5] == SP) else 5
        return {'ok': True, 'task': True, 'ordered': False, 'start': i, 'len': mlen, 'cells': 3}
    if rest >= 2 and line[i] in (DD, ST, PL) and line[i+1] == SP:
        return {'ok': True, 'task': False, 'ordered': False, 'start': i, 'len': 2, 'cells': 3}
    d = i
    while d < ln and 48 <= line[d] <= 57: d += 1
    nd = d - i
    if nd >= 1 and d + 2 < ln and line[d:d+3] == b'\xe3\x80\x81':  # 、
        mlen = nd + 3; mcells = nd + 3  # 内容右移1格,与无序一致
        if d + 3 < ln and line[d+3] == SP:
            mlen += 1; mcells += 1
        return {'ok': True, 'task': False, 'ordered': True, 'start': i, 'len': mlen, 'cells': mcells}
    if nd >= 1 and d < ln and line[d] in (ord('.'), ord(')')):
        if d + 1 >= ln: mlen = nd + 1          # "1." at EOL
        elif line[d+1] == SP: mlen = nd + 2    # "1. "
        else: return None                      # "1.5" → not a list
        return {'ok': True, 'task': False, 'ordered': True, 'start': i, 'len': mlen, 'cells': mlen + 1}
    # 中文序号 + 顿号:一、二、十、十一、… 原文渲染,前导1空格缩进
    c = i; nchars = 0
    while c + 3 <= ln and is_cn_num(line, c):
        c += 3; nchars += 1
    if nchars >= 1 and c + 2 < ln and line[c:c+3] == b'\xe3\x80\x81':  # 、
        mlen = (c - i) + 3; mcells = 1 + 2 * nchars + 2  # 前导1格 + 每字2格 + `、`2格
        if c + 3 < ln and line[c+3] == SP:
            mlen += 1; mcells += 1
        return {'ok': True, 'task': False, 'ordered': True, 'start': i, 'len': mlen, 'cells': mcells}
    return None

def md_visual_x(line, info, pos):
    cell = 1  # cells
    if info['inCodeBlock'] or info['hr']:
        return width(line[0:pos])
    if info['headingLevel']:
        n = info['headingLevel']
        if pos >= n:
            return 2 + md_content_width(line, n, pos)
        return 0
    if info['list'] or info['task']:
        m = md_list_marker(line)
        if m is not None:
            mend = m['start'] + m['len']
            if pos >= mend:
                return (m['start'] + m['cells']) + md_content_width(line, mend, pos)
            if pos <= m['start']:
                return md_content_width(line, 0, pos)
            return m['start'] + (pos - m['start'])  # inside marker (approx)
        return md_content_width(line, 0, pos)
    if info['quote']:
        if pos >= 2:
            return 4 + md_content_width(line, 2, pos)  # +2px bar offset not modeled
        return md_content_width(line, 0, pos)
    return md_content_width(line, 0, pos)

def md_vrow_x(line, info, pos, vrow_start):
    """Mirror of markdown_render.cpp mdVrowX: continuation vrows repeat the
    block prefix indent so wrapped content stays aligned under the first line."""
    prefix = 0
    if vrow_start > 0:
        if info['headingLevel'] > 0:
            prefix = 2
        elif info['quote']:
            prefix = 4
        elif info['list'] or info['task']:
            m = md_list_marker(line)
            if m is not None:
                prefix = m['start'] + m['cells']
    return md_visual_x(line, info, pos) - md_visual_x(line, info, vrow_start) + prefix

def md_content_width(line, frm, to):
    ln = len(line)
    if to > ln: to = ln
    if frm >= to: return 0
    px = 0
    i = frm
    while i < to:
        m = next_marker(line, i, to)
        if m >= to:
            px += width(line[i:to]); break
        if m > i:
            px += width(line[i:m]); i = m
        c = line[m]
        open_len = 1; close_idx = -1; close_len = 1; content_start = m + 1
        if c == ST:
            open_len = 1
            while m + open_len < ln and line[m + open_len] == ST: open_len += 1
            open_len = min(open_len, 3)
            cc = find_stars(line, m + open_len, ln, open_len)
            if cc >= 0:
                close_idx = cc; close_len = open_len; content_start = m + open_len
        elif c == BT:
            cc = line.find(b'`', m + 1)
            if cc >= 0:
                close_idx = cc; content_start = m + 1
        elif c == TL and m + 1 < ln and line[m + 1] == TL:
            cc = line.find(b'~~', m + 2)
            if cc >= 0:
                close_idx = cc; close_len = 2; content_start = m + 2; open_len = 2
        elif c == EQ and m + 1 < ln and line[m + 1] == EQ:
            cc = line.find(b'==', m + 2)
            if cc >= 0:
                close_idx = cc; close_len = 2; content_start = m + 2; open_len = 2
        elif c == LB:
            p = line.find(b'](', m + 1)
            if p >= 0 and p < ln:
                cp = line.find(b')', p + 2)
                if cp >= 0 and cp < ln:
                    link_end = cp + 1
                    if link_end >= to:
                        px += width(line[m:to]); break
                    px += width(line[m:link_end]); i = link_end; continue
        if close_idx < 0:
            px += 1; i = m + 1; continue
        open_end = m + open_len
        if to <= open_end:
            break
        close_end = close_idx + close_len
        if to <= close_idx:
            px += width(line[content_start:to]); break
        px += width(line[content_start:close_idx])
        i = close_end
    return px

def md_parse_line(line, info):
    segs = []
    ln = len(line)
    base = {}
    if info['headingLevel'] > 0:
        base['bold'] = True; base['underline'] = True
    if info['inCodeBlock']:
        st = dict(base); st['invert'] = True
        segs.append([0, ln, st, line])
        return segs
    if info['hr']:
        segs.append([0, ln, dict(base), spaces_for_width(line, 0, ln)])
        return segs
    pos = 0
    if info['headingLevel'] > 0:
        n = info['headingLevel']
        repl = HL_GLYPHS[n].encode('utf-8')
        while width(repl) < 2:  # heading glyph advance is 2 cells
            repl += b" "
        segs.append([0, n, dict(base), repl])
        pos = n
    elif info['list'] or info['task']:
        m = md_list_marker(line)
        if m is not None:
            mend = m['start'] + m['len']
            if m['task']:
                checked = (m['start'] + 3 < ln) and line[m['start'] + 3] in (ord('x'), ord('X'))
                repl = (" ✓ " if checked else " ☐ ").encode('utf-8')
                segs.append([0, mend, dict(base), line[0:m['start']] + repl])
            elif m['ordered']:
                nst = dict(base); nst['bold'] = True
                segs.append([0, mend, nst, b" " + line[0:mend]])  # 前导补1格,序号随整体右移1格,与无序一致
            else:
                segs.append([0, mend, dict(base), line[0:m['start']] + b" \xe2\x80\xa2 "])  # bullet
            pos = mend
    elif info['quote']:
        segs.append([0, 2, dict(base), b"    "])  # bar at cell 4, 1-space gap, content at cell 5
        pos = 2
    md_parse_inline(line, pos, base, segs)
    return segs

def md_classify_lines(lines):
    out = []
    in_code = False
    for ln in lines:
        info = {'headingLevel': 0, 'list': False, 'task': False, 'quote': False,
                'inCodeBlock': False, 'hr': False}
        end = len(ln)
        while end > 0 and ln[end-1] in (SP, ord('\t')): end -= 1
        if ln[0:3] == b'```':
            info['hr'] = True
            in_code = not in_code
            out.append(info)
            continue
        if in_code:
            info['inCodeBlock'] = True
            out.append(info)
            continue
        ws = 0
        while ws < end and ln[ws] in (SP, ord('\t')): ws += 1
        # heading / quote only at column 0; horizontal rule tolerates leading ws
        if ws == 0:
            h = 0
            while h < end and ln[h] == HT: h += 1
            if 1 <= h <= 6 and h < end and ln[h] == SP:
                info['headingLevel'] = h
                out.append(info)
                continue
        if end >= 3:
            hr = True; cnt = 0
            for k in range(end):
                c = ln[k]
                if c in (SP, ord('\t')): continue
                if c in (DD, ST, US): cnt += 1; continue
                hr = False; break
            if hr and cnt >= 3:
                info['hr'] = True
                out.append(info)
                continue
        if ws == 0 and end >= 2 and ln[0] == GT and ln[1] == SP:
            info['quote'] = True
            out.append(info)
            continue
        # list family (unordered/ordered/task), allowed after leading ws = nested
        m = md_list_marker(ln)
        if m is not None:
            info['task'] = m['task']
            info['list'] = True
        out.append(info)
    return out

def check(line, info):
    ln = len(line)
    segs = md_parse_line(line, info)
    prev = 0
    for s, e, st, dr in segs:
        assert s == prev, f"gap at byte {prev} in {line!r}: seg [{s},{e})"
        assert e > s, f"empty seg [{s},{e}) in {line!r}"
        prev = e
    assert prev == ln, f"uncovered tail {prev}/{ln} in {line!r}"
    for s, e, st, dr in segs:
        vs = md_visual_x(line, info, s)
        ve = md_visual_x(line, info, e)
        assert vs + width(dr) == ve, \
            f"VISUAL MISMATCH {line!r}: seg[{s},{e}) start={vs} draw={width(dr)} end={ve} ({dr!r})"
    return segs

# Mirror of ui_helpers.cpp mdPrefixLen (bytes).
def md_prefix_len(line):
    m = md_list_marker(line)
    if m is not None: return m['start'] + m['len']
    if line[0:2] == b'> ': return 2
    h = 0
    while h < len(line) and h < 6 and line[h:h+1] == b'#': h += 1
    if h >= 1 and h < len(line) and line[h:h+1] == b' ': return h + 1
    return 0

# Mirror of ui_helpers.cpp mdIndentCells: cells reserved per vrow for the
# RENDERED block indent so wrapped content doesn't overrun the screen.
def md_indent(line):
    m = md_list_marker(line)
    if m is not None: return m['start'] + m['cells']
    if line[0:2] == b'> ': return 4
    return 2 if md_prefix_len(line) > 0 else 0

# Mirror of ui_helpers.cpp buildVrows (incl. mdIndentCells): reserves indent
# cells for markdown block lines and never splits a leading marker.
def build_vrows(line):
    maxc = 400 // 14  # SCREEN_W / halfAdvance @28pt
    indent_cells = md_indent(line)
    prefix_end = md_prefix_len(line)
    vrows = []
    pos = 0
    while pos < len(line):
        cells = 0; end = pos; last = -1
        pe = prefix_end if pos == 0 else 0
        # cap 用标记格数(byteToCells)而非字节数,镜像 C++:CJK 标记字节>格数
        marker_cells = width(line[0:prefix_end]) if pos == 0 else 0
        cap = maxc - indent_cells + marker_cells
        if cap > maxc: cap = maxc
        while end < len(line):
            n = utf8_len(line[end])
            cc = 1 if line[end] < 0x80 else 2  # charCellWidth
            if cells + cc > cap: break
            cells += cc
            if line[end] == SP and end >= pe: last = end + 1
            end += n
        if end >= len(line):
            vrows.append((pos, len(line))); break
        if last > pos:
            vrows.append((pos, last)); pos = last
            while pos < len(line) and line[pos] == SP: pos += 1
        else:
            vrows.append((pos, end)); pos = end
    return vrows


TEST_LINES = [
    "# 标题",
    "## 二级标题",
    "###### 六级标题",
    "普通文本行",
    "**加粗**",
    "*斜体*",
    "***粗斜体***",
    "`代码`",
    "==高亮==",
    "~~删除~~",
    "- 列表项",
    "* 星号列表",
    "+ 加号列表",
    "- [ ] 待办事项",
    "- [x] 已完成任务",
    "1. 有序第一项",
    "10. 第十项",
    "1、中文序号",
    "1) 右括号列表",
    "1.5 不是列表",
    "2024年回顾",
    "  - 嵌套列表",
    "  * 嵌套星号",
    "  - [x] 嵌套任务",
    "  - [ ] 嵌套待办",
    "  1. 嵌套有序",
    "  3、嵌套中文序号",
    "10、第十项",
    "11、第十一项",
    "20、第二十项",
    "一、直接中文序号",
    "十二、直接中文十二",
    "100、超过百项",
    "    - 深嵌套列表",
    "  1.5 不是嵌套有序",
    "> 引用内容",
    "---",
    "***",
    "___",
    "[链接文本](https://example.com)",
    "\\*转义星号\\*",
    "混合 **加粗** 和 `代码` 以及 ==高亮== 一行",
    "- [ ] 混合 **粗** ==高== 任务",
    "> 引用 **内嵌** 内容",
    "中文句子，包含,，标点符号。",
    "### 标题带 `代码` 和 [链接](http://x)",
    "普通长文本行会进行折行显示，包含一些中文内容测试宽度计算是否正确。",
    "## `代码标题`",
    "",
    "   ",
    "# ",
    "- ",
    "- [ ]",
    "```cpp",
    "code line 1",
    "int x = 1;",
    "```",
    "a === b",
]

def main():
    lines = [l.encode('utf-8') for l in TEST_LINES]
    infos = md_classify_lines(lines)
    fails = 0
    for line, info in zip(lines, infos):
        try:
            segs = check(line, info)
            flags = "".join(" " if not v else ("H" if k == 'headingLevel' else k[0].upper())
                            for k, v in info.items())
            print(f"OK   {line.decode('utf-8'):<36} {flags}")
        except AssertionError as e:
            fails += 1
            print(f"FAIL {line.decode('utf-8'):<36} {info}\n      {e}")
    # layout assertions: list/task content at start+cells, quote content at cell 4
    for line, info in zip(lines, infos):
        if info['task'] or info['list']:
            m = md_list_marker(line)
            assert m is not None, f"{line!r} classified as list/task but no marker"
            mend = m['start'] + m['len']
            cx = m['start'] + m['cells']
            assert md_visual_x(line, info, mend) == cx, \
                f"list content not at cell {cx}: {line!r} got {md_visual_x(line, info, mend)}"
            assert md_visual_x(line, info, len(line)) == cx + md_content_width(line, mend, len(line)), \
                f"list tail misaligned: {line!r}"
        if info['quote']:
            assert md_visual_x(line, info, 2) == 4, f"quote content not at cell 4: {line!r}"
            assert md_visual_x(line, info, len(line)) == 4 + md_content_width(line, 2, len(line)), \
                f"quote tail misaligned: {line!r}"
    # wrap tests: a leading markdown marker must never split onto its own vrow
    # (that rendered as a bar / empty row), and continuation vrows carry text.
    for wl in ["> ", "- ", "- [ ] ", "# ", "1. ", "1、 ", "  - ", "  1. ", "  - [ ] "]:
        b = (wl + "内容很长需要折行显示" * 6).encode('utf-8')
        inf = md_classify_lines([b])[0]
        vrows = build_vrows(b)
        assert len(vrows) > 1, f"expected multi-vrow wrap: {wl!r}"
        for s, e in vrows:
            if s == 0:
                assert e > md_prefix_len(b), f"marker split onto own vrow: {wl!r} [{s},{e})"
            else:
                drawn = [dr for st, en, ts, dr in md_parse_line(b, inf) if en > s and st < e]
                assert any(dr.strip() for dr in drawn), \
                    f"continuation vrow empty: {wl!r} [{s},{e})"
                if inf['list'] or inf['task'] or inf['quote'] or inf['headingLevel']:
                    if inf['headingLevel']: expect = 2
                    elif inf['quote']: expect = 4
                    else:
                        mm = md_list_marker(b)
                        expect = mm['start'] + mm['cells']
                    got = md_vrow_x(b, inf, s, s)
                    assert got == expect, \
                        f"continuation vrow x wrong: {wl!r} [{s},{e}) got {got} want {expect}"
    print("wrap split tests OK")
    # fence sanity: the 4 ```-related lines should toggle correctly
    fence = [i for i, l in enumerate(lines) if l[0:3] == b'```']
    assert len(fence) == 2, "expected exactly 2 fence lines"
    assert infos[fence[0]]['hr'] and infos[fence[1]]['hr'], "fence lines not hr"
    assert infos[fence[0]+1]['inCodeBlock'], "code after opener not inCodeBlock"
    assert infos[fence[0]+2]['inCodeBlock'], "code content before close fence"
    assert not infos[fence[1]]['inCodeBlock'], "closing fence should not be inCodeBlock"
    assert not infos[fence[1]+1]['inCodeBlock'], "line after close fence not code"
    print("\nfence toggle OK")
    print(f"{len(lines) - fails}/{len(lines)} width checks passed, {fails} failed")
    return 1 if fails else 0

if __name__ == '__main__':
    raise SystemExit(main())
