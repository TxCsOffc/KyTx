import sys, glob, os, re
def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    off = m.end()
    return w, h, data[off:off + w * h * 3]
rows = []
for p in sorted(glob.glob(os.path.join(sys.argv[1], '*.ppm'))):
    r = read_ppm(p)
    if r is None:
        continue
    w, h, px = r
    n = w * h
    if n == 0 or len(px) < n * 3:
        continue
    # sample every 7th pixel for speed
    step = 7
    nz = 0; cnt = 0; sr = sg = sb = 0
    for i in range(0, n, step):
        o = i * 3
        r_, g_, b_ = px[o], px[o+1], px[o+2]
        cnt += 1
        if r_ or g_ or b_:
            nz += 1
        sr += r_; sg += g_; sb += b_
    rows.append((nz / max(cnt, 1), sr / max(cnt,1), sg / max(cnt,1), sb / max(cnt,1), os.path.basename(p), w, h))
rows.sort(key=lambda t: -t[0])
print(f"{'nonzero%':>8} {'meanR':>6} {'meanG':>6} {'meanB':>6}  file")
for nzr, mr, mg, mb, name, w, h in rows:
    print(f"{nzr*100:8.1f} {mr:6.1f} {mg:6.1f} {mb:6.1f}  {name}")
