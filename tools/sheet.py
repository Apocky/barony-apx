# tools/sheet.py -- the F3 contact sheet: 6 dressed floors (one per theme) and the 12 fixtures.
# Reads apxgen --dump files and the .vox files with its own parser (independent of the C++ reader),
# writes out/sheet/sheet.png and out/sheet/sheet.txt, and prints the .vox parse-back table.
#   python tools/sheet.py out/dressed/<theme>-<seed>.txt ... --vox out/vox --out out/sheet
import os
import struct
import sys

from PIL import Image, ImageDraw

args = sys.argv[1:]
vox_dir = args[args.index('--vox') + 1]
out_dir = args[args.index('--out') + 1]
dumps = [a for a in args if a.endswith('.txt')]
os.makedirs(out_dir, exist_ok=True)

FIXTURES = ['table', 'bench', 'shelf', 'barrel', 'crate', 'brazier', 'altar', 'bed', 'rack', 'pillar', 'statue', 'rubble']
FIX_COL = {'table': (196, 140, 70), 'bench': (170, 120, 60), 'shelf': (120, 80, 40), 'barrel': (150, 100, 50), 'crate': (210, 170, 90),
           'brazier': (255, 140, 20), 'altar': (230, 230, 240), 'bed': (220, 210, 180), 'rack': (110, 110, 130), 'pillar': (200, 200, 190),
           'statue': (170, 180, 170), 'rubble': (110, 105, 100)}
ZONE_COL = {'1': (128, 156, 120), '2': (150, 142, 124), '3': (132, 118, 100), '4': (160, 110, 100)}
FEAT_COL = {'~': (60, 110, 180), '=': (20, 40, 110), '^': (230, 90, 10), ':': (12, 6, 20), '"': (150, 190, 220), '+': (120, 84, 50)}


def parse_dump(path):
    lines = open(path).read().split('\n')
    head = lines[0].split()
    theme = head[head.index('theme') + 1]
    seed = head[1]
    summary = lines[1]
    grid = lines[4:4 + 128]
    zones, dress, ents = [], [], []
    for l in lines:
        if l.startswith('zone '):
            zones.append(l[5:])
        elif l.startswith('dress '):
            p = l.split()
            dress.append(dict(cls=p[1], id=p[2], v=int(p[3]), x=int(p[4]), z=int(p[5]), rot=int(p[6]), zone=p[7]))
        elif l.startswith('entity '):
            p = l.split()
            x, z = p[3].split(',')
            ents.append((p[1], int(x), int(z)))
    return dict(theme=theme, seed=seed, summary=summary, grid=grid, zones=zones, dress=dress, ents=ents, path=path)


def read_vox(path):
    b = open(path, 'rb').read()
    sx, sy, sz = struct.unpack('<iii', b[:12])
    n = sx * sy * sz
    data = b[12:12 + n]
    pal = b[12 + n:12 + n + 768]
    assert len(pal) == 768 and len(b) == 12 + n + 768, path
    return sx, sy, sz, data, pal


FX = [0, 1, 0, -1]
FZ = [1, 0, -1, 0]
FOOT = {'table': (2, 1), 'altar': (2, 1), 'bed': (1, 2)}


def render_floor(F, px=3):
    W = H = 128
    img = Image.new('RGB', (W * px, H * px), (24, 24, 28))
    d = ImageDraw.Draw(img)
    for z in range(H):
        row = F['grid'][z]
        for x in range(W):
            c = row[x]
            col = None
            if c == '#':
                col = (8, 8, 10)
            elif c in FEAT_COL:
                col = FEAT_COL[c]
            elif c != ' ':
                zc = F['zones'][z][x] if F['zones'] else '0'
                col = ZONE_COL.get(zc, (96, 92, 84))  # corridor floor
                if c.isdigit():
                    col = tuple(int(v * 0.85) for v in col)
            if col:
                d.rectangle([x * px, z * px, x * px + px - 1, z * px + px - 1], fill=col)
    for r in F['dress']:
        if r['cls'] == 'fixture':
            fw, fd = FOOT.get(r['id'], (1, 1))
            rx, rz = FZ[r['rot']], -FX[r['rot']]
            for a in range(fw):
                for b in range(fd):
                    x = r['x'] + a * rx + b * FX[r['rot']]
                    z = r['z'] + a * rz + b * FZ[r['rot']]
                    d.rectangle([x * px, z * px, x * px + px - 1, z * px + px - 1], fill=FIX_COL.get(r['id'], (255, 0, 255)))
        elif r['cls'] == 'prop':
            d.point((r['x'] * px + 1, r['z'] * px + 1), fill=(250, 240, 200))
        else:  # wall trim: a tick on the wall side of the tile
            k = r['rot']
            cx, cz = r['x'] * px + 1 + FX[k], r['z'] * px + 1 + FZ[k]
            d.point((cx, cz), fill=(240, 200, 60))
    for kind, x, z in F['ents']:
        col = {'stairs_up': (80, 255, 80), 'stairs_down': (80, 255, 80), 'boss': (255, 40, 40), 'key': (255, 230, 0), 'lock_door': (255, 230, 0)}.get(kind, (80, 200, 255))
        d.rectangle([x * px - 1, z * px - 1, x * px + px, z * px + px], outline=col)
    return img


def render_vox(path, s=4):
    sx, sy, sz, data, pal = read_vox(path)
    # front view (x across, height up, nearest y wins) and top view (x across, y down, highest wins)
    front = Image.new('RGB', (sx * s, sz * s), (30, 30, 36))
    top = Image.new('RGB', (sx * s, sy * s), (30, 30, 36))
    df, dt = ImageDraw.Draw(front), ImageDraw.Draw(top)
    count = 0
    for x in range(sx):
        for y in range(sy):
            for z in range(sz):
                if data[z + y * sz + x * sy * sz] != 255:
                    count += 1
    for x in range(sx):
        for zb in range(sz):  # zb: Barony index; 0 = top
            for y in range(sy):
                c = data[zb + y * sz + x * sy * sz]
                if c != 255:
                    r, g, b = (pal[c * 3] << 2, pal[c * 3 + 1] << 2, pal[c * 3 + 2] << 2)
                    sh = 1.0 - 0.35 * y / max(1, sy - 1)
                    df.rectangle([x * s, zb * s, x * s + s - 1, zb * s + s - 1], fill=(int(r * sh), int(g * sh), int(b * sh)))
                    break
        for y in range(sy):
            for zb in range(sz):
                c = data[zb + y * sz + x * sy * sz]
                if c != 255:
                    r, g, b = (pal[c * 3] << 2, pal[c * 3 + 1] << 2, pal[c * 3 + 2] << 2)
                    sh = 1.0 - 0.45 * zb / max(1, sz - 1)
                    dt.rectangle([x * s, y * s, x * s + s - 1, y * s + s - 1], fill=(int(r * sh), int(g * sh), int(b * sh)))
                    break
    return front, top, (sx, sy, sz, count)


def ascii_vox(path):
    sx, sy, sz, data, pal = read_vox(path)
    rows = []
    for zb in range(sz):
        row = ''
        for x in range(sx):
            row += '#' if any(data[zb + y * sz + x * sy * sz] != 255 for y in range(sy)) else '.'
        rows.append(row)
    return rows


floors = [parse_dump(p) for p in dumps]
PX = 3
cols = 3
fw = 128 * PX
label_h = 30
sheet_w = cols * (fw + 10) + 10
rows_f = (len(floors) + cols - 1) // cols
props = []
for fid in FIXTURES:
    front, top, info = render_vox(os.path.join(vox_dir, fid + '.vox'))
    props.append((fid, front, top, info))
cell_w = (sheet_w - 10) // 6
prop_h = max(p[1].height + p[2].height for p in props) + 40
sheet_h = 40 + rows_f * (fw + label_h + 10) + 2 * prop_h + 20
sheet = Image.new('RGB', (sheet_w, sheet_h), (16, 16, 20))
d = ImageDraw.Draw(sheet)
d.text((10, 10), 'apx-gen F3: six dressed floors (one per theme) and the 12 fixture props (.vox front + top, read back from disk)', fill=(230, 230, 230))
for i, F in enumerate(floors):
    x0 = 10 + (i % cols) * (fw + 10)
    y0 = 40 + (i // cols) * (fw + label_h + 10)
    s = F['summary'].split()
    kv = dict(zip(s[1::2], s[2::2]))
    d.text((x0, y0), '%s  seed %s  items %s  dens %.1f/10m2' % (F['theme'], F['seed'], kv['items'], float(kv['density_per_10m2'])), fill=(230, 230, 200))
    d.text((x0, y0 + 13), 'clone %s  neg-space rooms %.0f%%  3+ kinds %s' % (kv['clone_groups'], float(kv['neg_space_rooms']), kv['three_kinds']), fill=(180, 180, 160))
    sheet.paste(render_floor(F, PX), (x0, y0 + label_h))
py = 40 + rows_f * (fw + label_h + 10)
for i, (fid, front, top, info) in enumerate(props):
    x0 = 10 + (i % 6) * cell_w
    y0 = py + (i // 6) * prop_h
    d.text((x0, y0), '%s %dx%dx%d %d vox' % (fid, info[0], info[1], info[2], info[3]), fill=(230, 230, 200))
    sheet.paste(front, (x0, y0 + 14))
    sheet.paste(top, (x0, y0 + 18 + front.height))
sheet.save(os.path.join(out_dir, 'sheet.png'))

with open(os.path.join(out_dir, 'sheet.txt'), 'w', newline='\n') as o:
    o.write('apx-gen F3 contact sheet (ASCII). Legend: see the second and third lines of each floor.\n\n')
    for F in floors:
        src = open(F['path']).read().split('\n')
        o.write('\n'.join(src[:4 + 128]) + '\n\n')
    o.write('The 12 fixtures, front silhouettes from the .vox files (x across, Barony z index 0 at the top)\n\n')
    for fid in FIXTURES:
        o.write('%s\n' % fid)
        o.write('\n'.join(ascii_vox(os.path.join(vox_dir, fid + '.vox'))) + '\n\n')

print('vox parse-back (python reader):')
for fid, front, top, info in props:
    print('  %-8s dims %dx%dx%d voxels %d' % (fid, info[0], info[1], info[2], info[3]))
print('wrote', os.path.join(out_dir, 'sheet.png'), sheet.size, 'and sheet.txt')
