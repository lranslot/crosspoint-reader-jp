# -*- coding: utf-8 -*-
"""Design the bouten marks as explicit bitmaps, measure them, and render the
mock from the same tables the firmware will carry. One source for both."""
import io, sys, zlib, struct
sys.stdout.reconfigure(encoding='utf-8')

# Hand-tuned rather than generated: at 3-9 px a midpoint circle produces
# lopsided rings (one row 4 px wide, the next 2), and these are read at 157 dpi.
# Row strings, '#' = ink. Filled first, then hollow.
FILLED = {
    3: ['.#.', '###', '.#.'],
    4: ['.##.', '####', '####', '.##.'],
    5: ['.###.', '#####', '#####', '#####', '.###.'],
    6: ['.####.', '######', '######', '######', '######', '.####.'],
    7: ['..###..', '.#####.', '#######', '#######', '#######', '.#####.', '..###..'],
    8: ['..####..', '.######.', '########', '########', '########', '########', '.######.', '..####..'],
    9: ['..#####..', '.#######.', '#########', '#########', '#########', '#########', '#########',
        '.#######.', '..#####..'],
}
HOLLOW = {
    3: ['.#.', '#.#', '.#.'],
    4: ['.##.', '#..#', '#..#', '.##.'],
    5: ['.###.', '#...#', '#...#', '#...#', '.###.'],
    6: ['.####.', '#....#', '#....#', '#....#', '#....#', '.####.'],
    7: ['..###..', '.#...#.', '#.....#', '#.....#', '#.....#', '.#...#.', '..###..'],
    8: ['..####..', '.#....#.', '#......#', '#......#', '#......#', '#......#', '.#....#.', '..####..'],
    9: ['..#####..', '.#.....#.', '#.......#', '#.......#', '#.......#', '#.......#', '#.......#',
        '.#.....#.', '..#####..'],
}

EM = {8: 17, 10: 21, 12: 25, 14: 29, 16: 33, 18: 38}
RATIO = {'狭い': 115, '普通': 130, '広い': 150}


def dot_for(cell):
    """The firmware rule: cell / 4, floored, never under 3, never over 9."""
    return max(3, min(9, cell // 4))


def measure(rows):
    d = len(rows)
    ink = sum(r.count('#') for r in rows)
    mid = rows[d // 2]
    hole = mid.count('.') - (len(mid) - len(mid.rstrip('.')) ) - (len(mid) - len(mid.lstrip('.')))
    # hole: the run of '.' strictly inside the ink on the middle row
    inner = mid.strip('.')
    hole = inner.count('.')
    return d, ink, hole


print('【点の寸法】outer = 外径 (px, 正方)  ink = 打つ点の数  hole = 中央行の穴の幅')
print()
print('本文pt  升目  点の外径  輪郭  穴(中央行)  塗りの ink  白丸の ink')
for pt in (12, 14, 16, 18):
    cell = EM[pt]
    d = dot_for(cell)
    _, fink, _ = measure(FILLED[d])
    _, hink, hhole = measure(HOLLOW[d])
    print('%5d  %4d  %7d  %4d  %9d  %10d  %10d' % (pt, cell, d, 1, hhole, fink, hink))
print()
print('【帯と送り】band = cellSize + 点の外径 + 1')
print('本文pt  升目  点  帯の送り  通常の送り(狭い/普通/広い)  傍点の送り(狭い/普通/広い)')
for pt in (12, 14, 16, 18):
    cell = EM[pt]
    d = dot_for(cell)
    norm = [cell * r // 100 for r in RATIO.values()]
    bout = [max(n, cell + d + 1) for n in norm]
    marks = ['吸収' if b == n else '+%d' % (b - n) for b, n in zip(bout, norm)]
    print('%5d  %4d  %2d  %8d  %-22s  %s' % (pt, cell, d, cell + d + 1,
          '/'.join(map(str, norm)), '/'.join('%d(%s)' % (b, m) for b, m in zip(bout, marks))))

# ---------------- the C table ----------------
print()
print('【生成した C のテーブル】')
lines = []
for name, tbl in (('kBoutenFilled', FILLED), ('kBoutenHollow', HOLLOW)):
    lines.append('// %s[d - kBoutenMinSize]: one byte per row, bit 0 is the leftmost pixel.' % name)
    lines.append('static constexpr uint16_t %s[][9] = {' % name)
    for d in range(3, 10):
        rows = tbl[d]
        vals = []
        for r in rows:
            v = 0
            for i, c in enumerate(r):
                if c == '#':
                    v |= 1 << i
            vals.append('0x%03X' % v)
        while len(vals) < 9:
            vals.append('0x000')
        lines.append('    {%s},  // %d px' % (', '.join(vals), d))
    lines.append('};')
code = '\n'.join(lines)
print(code)
io.open('bouten_table.txt', 'w', encoding='utf-8', newline='\n').write(code + '\n')


# ---------------- the PNG mock ----------------
def png(path, pixels, w, h):
    raw = b''.join(b'\x00' + bytes(255 if not p else 0 for p in pixels[y * w:(y + 1) * w])
                   for y in range(h))
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    out = b'\x89PNG\r\n\x1a\n'
    out += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
    out += chunk(b'IDAT', zlib.compress(raw, 9))
    out += chunk(b'IEND', b'')
    io.open(path, 'wb').write(out)


# layout: for each body size, a row holding  1x filled, 1x hollow, 8x filled, 8x hollow
SCALE = 8
PAD = 6
W, H = 560, 4 * (9 * SCALE + 2 * PAD) + PAD
buf = [0] * (W * H)


def put(rows, ox, oy, scale):
    for y, r in enumerate(rows):
        for x, c in enumerate(r):
            if c != '#':
                continue
            for dy in range(scale):
                for dx in range(scale):
                    px, py = ox + x * scale + dx, oy + y * scale + dy
                    if 0 <= px < W and 0 <= py < H:
                        buf[py * W + px] = 1


y = PAD
for pt in (12, 14, 16, 18):
    cell = EM[pt]
    d = dot_for(cell)
    row_h = 9 * SCALE
    # actual size, with a cell-sized frame of ink dots marking the baseline band
    put(FILLED[d], PAD, y + row_h // 2, 1)
    put(HOLLOW[d], PAD + 20, y + row_h // 2, 1)
    # three in a row at actual size, spaced one cell apart (what a word looks like)
    for k in range(3):
        put(HOLLOW[d], PAD + 50 + k * cell, y + row_h // 2, 1)
    put(FILLED[d], 220, y, SCALE)
    put(HOLLOW[d], 220 + 10 * SCALE, y, SCALE)
    y += row_h + 2 * PAD

png('../docs-jp/mockups/f8-bouten-marks.png', buf, W, H)
print()
print('PNG: ../docs-jp/mockups/f8-bouten-marks.png  (%dx%d, 左から 実寸の塗り / 実寸の白丸 / 実寸で3つ並べたもの / 8倍の塗り / 8倍の白丸、行は上から 12/14/16/18pt)' % (W, H))
