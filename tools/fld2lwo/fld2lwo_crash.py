#!/usr/bin/env python3
"""Recover the shipping-revision lt_scr.lwo (LWO1) from the shipping CRASH.FLD.

The crash (laptop "END") scene ships lt_scr.lwo with the screen panel
recessed by dZ=-1.5 (12 verts) vs the vintage Original/.../END/LT_SCR.LWO;
that newer revision exists nowhere in the source tree, so recover it from
the FLD exactly as the city b1/b3/b6 buildings were recovered.

Inverts tools/lwsread's LWOREAD.CPP transforms bit-exactly:
  PNTS   : FLD (X,Y,Z) LE  ->  LWO BE floats (X, Z, Y)   [SwapYZ inverse, bit copy]
  POLS   : FLD u16 face lists verbatim, surface index +1, BE
  SRFS   : FLD material order, NUL-terminated names padded to even
  SURF   : per-field subchunks; straight floats bit-copied BE, *100 fields
           inverted in float32 with round-trip assertion (legacy VLUM*100)
Self-check: simulates the reader on the emitted bytes and compares the
resulting FLD-record bytes (materials+verts+faces) against the shipping blob.
"""
import struct, sys, math

FLD = 'Runtime/SCENES/CRASH.FLD'
TARGETS = {'lt_scr.lwo': 0}

# ---------------- FLD walk (byte spans) ----------------
def walk(path):
    d = open(path, 'rb').read(); n = len(d); p = [0]
    def u32():
        v = struct.unpack_from('<I', d, p[0])[0]; p[0] += 4; return v
    def u16():
        v = struct.unpack_from('<H', d, p[0])[0]; p[0] += 2; return v
    def cstr():
        s = p[0]
        while d[p[0]] != 0: p[0] += 1
        v = d[s:p[0]]; p[0] += 1; return v.decode('latin-1')
    def env():
        keys = u32(); ch = u32(); u32()
        p[0] += keys * (ch * 4 + 20)
    assert d[:7] == b'Flood3D'; p[0] = 7
    p[0] += 4
    u32(); u32(); u32(); p[0] += 4
    env(); env(); p[0] += 3
    objs = []
    def material():
        cstr(); p[0] += 3 + 2 + 20 + 4; cstr(); p[0] += 16
        for _ in range(7): cstr()
        p[0] += 2 + 48; cstr(); p[0] += 6 + 16
    while p[0] < n:
        start = p[0]; cid = u16()
        if cid == 0x1000:
            name = cstr(); ofl = u32(); nmat = u32()
            mat0 = p[0]
            for _ in range(nmat): material()
            mat1 = p[0]
            nv = u32(); v0 = p[0]; p[0] += nv * 12
            nf = u32(); f0 = p[0]
            for _ in range(nf):
                fv = u16(); p[0] += fv * 2 + 2
            f1 = p[0]
            keys = u32(); p[0] += keys * 56
            if ofl & 1: p[0] += 4
            if ofl & 2: p[0] += 12
            if ofl & 4: env()
            objs.append(dict(name=name, mat=(mat0, mat1), nmat=nmat,
                             nv=nv, verts=(v0, v0 + nv * 12), nf=nf, faces=(f0, f1)))
        elif cid == 0x2000:
            cstr(); lfl = u32(); keys = u32(); p[0] += keys * 56; p[0] += 3
            env()
            if lfl & 1: p[0] += 4
            if lfl & 2: p[0] += 4
            if lfl & 4: env()
            if lfl & 8: env()
            if lfl & 16: env()
        elif cid == 0x3000:
            cfl = u32(); keys = u32(); p[0] += keys * 56
            env()
            if cfl & 1: p[0] += 4
            if cfl & 2: p[0] += 4
        else:
            raise SystemExit(f'bad chunk at {start}')
    return d, objs

# ---------------- material record parse ----------------
FIELD_ORDER = [  # (key, kind) in FLD record order
    ('name', 's'), ('color', 'b3'), ('flags', 'u2'),
    ('lum', 'f'), ('diff', 'f'), ('spec', 'f'), ('refl', 'f'), ('trans', 'f'),
    ('gloss', 'u2'), ('refmode', 'u2'), ('refimg', 's'),
    ('seam', 'f'), ('refidx', 'f'), ('edge', 'f'), ('sman', 'f'),
    ('ctex', 's'), ('dtex', 's'), ('stex', 's'), ('rtex', 's'),
    ('ttex', 's'), ('btex', 's'), ('timg', 's'),
    ('tflg', 'u2'), ('tsiz', 'v'), ('tctr', 'v'), ('tfal', 'v'), ('tvel', 'v'),
    ('talp', 's'), ('tfrq', 'u2'), ('wrapx', 'u2'), ('wrapy', 'u2'),
    ('aa', 'f'), ('opac', 'f'), ('tfp0', 'f'), ('tfp1', 'f'),
]

def parse_mats(d, lo, nmat):
    p = [lo]; mats = []
    def cstr():
        s = p[0]
        while d[p[0]] != 0: p[0] += 1
        v = d[s:p[0]]; p[0] += 1; return v
    for _ in range(nmat):
        m = {}
        for key, kind in FIELD_ORDER:
            if kind == 's':
                m[key] = cstr()
            elif kind == 'b3':
                m[key] = d[p[0]:p[0] + 3]; p[0] += 3
            elif kind == 'u2':
                m[key] = struct.unpack_from('<H', d, p[0])[0]; p[0] += 2
            elif kind == 'f':
                m[key] = d[p[0]:p[0] + 4]; p[0] += 4      # raw LE bytes
            elif kind == 'v':
                m[key] = d[p[0]:p[0] + 12]; p[0] += 12    # raw LE 3 floats
        mats.append(m)
    return mats

# ---------------- inversion helpers ----------------
def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]

def le2be(raw4):                      # bit-exact float endian flip
    return raw4[::-1]

def vec_le_to_lwo_be(raw12):          # (X,Y,Z) LE -> (X,Y,Z) BE (SwapYZ is a NO-OP in LWSREAD.CPP)
    x, y, z = raw12[0:4], raw12[4:8], raw12[8:12]
    return x[::-1] + y[::-1] + z[::-1]

def invert_x100(raw4, what):
    """Find float32 Temp with float32(Temp*100) bit-equal to raw4 (LE)."""
    target = struct.unpack('<f', raw4)[0]
    base = f32(target / 100.0)
    cand = [base]
    for k in range(1, 9):
        cand.append(f32(math.nextafter(base, math.inf)) if k % 2 else base)
    # widen search: walk ULPs both directions
    up = dn = base
    for _ in range(16):
        up = math.nextafter(up, math.inf); dn = math.nextafter(dn, -math.inf)
        cand += [f32(up), f32(dn)]
    for t in cand:
        if struct.pack('<f', f32(t * 100.0)) == raw4:
            return struct.pack('>f', t)
    raise SystemExit(f'cannot invert *100 for {what}: {target!r}')

def is_zero_f(raw4): return struct.unpack('<f', raw4)[0] == 0.0
def is_zero_v(raw12): return raw12 == b'\x00' * 12

# ---------------- LWO writers ----------------
def padstr(b):                         # NUL-terminated, padded to even
    s = b + b'\x00'
    return s + (b'\x00' if len(s) & 1 else b'')

def sub(cid, payload):
    assert len(payload) < 0x10000
    return cid + struct.pack('>H', len(payload)) + payload + (b'\x00' if len(payload) & 1 else b'')

def chunk(cid, payload):
    return cid + struct.pack('>I', len(payload)) + payload + (b'\x00' if len(payload) & 1 else b'')

def build_surf(m):
    out = padstr(m['name'])
    if m['color'] != b'\x00\x00\x00':
        out += sub(b'COLR', m['color'] + b'\x00')
    if m['flags']:
        out += sub(b'FLAG', struct.pack('>H', m['flags']))
    if not is_zero_f(m['lum']):
        out += sub(b'VLUM', invert_x100(m['lum'], 'lum'))     # legacy: *100
    if not is_zero_f(m['diff']):
        out += sub(b'VDIF', invert_x100(m['diff'], 'diff'))
    if not is_zero_f(m['spec']):
        out += sub(b'VSPC', invert_x100(m['spec'], 'spec'))
    if not is_zero_f(m['refl']):
        out += sub(b'VRFL', invert_x100(m['refl'], 'refl'))
    if not is_zero_f(m['trans']):
        out += sub(b'VTRN', invert_x100(m['trans'], 'trans'))
    if m['gloss']:
        out += sub(b'GLOS', struct.pack('>H', m['gloss']))
    if m['refmode']:
        out += sub(b'RFLT', struct.pack('>H', m['refmode']))
    if m['refimg']:
        out += sub(b'RIMG', padstr(m['refimg']))
    if not is_zero_f(m['seam']):
        out += sub(b'RSAN', le2be(m['seam']))
    if not is_zero_f(m['refidx']):
        out += sub(b'RIND', le2be(m['refidx']))
    if not is_zero_f(m['edge']):
        out += sub(b'EDGE', le2be(m['edge']))
    if not is_zero_f(m['sman']):
        out += sub(b'SMAN', le2be(m['sman']))
    for key, cid in (('ctex', b'CTEX'), ('dtex', b'DTEX'), ('stex', b'STEX'),
                     ('rtex', b'RTEX'), ('ttex', b'TTEX'), ('btex', b'BTEX')):
        if m[key]:
            out += sub(cid, padstr(m[key]))
    if m['timg']:
        out += sub(b'TIMG', padstr(m['timg']))
    if m['tflg']:
        out += sub(b'TFLG', struct.pack('>H', m['tflg']))
    for key, cid in (('tsiz', b'TSIZ'), ('tctr', b'TCTR'),
                     ('tfal', b'TFAL'), ('tvel', b'TVEL')):
        if not is_zero_v(m[key]):
            out += sub(cid, vec_le_to_lwo_be(m[key]))
    if m['talp']:
        out += sub(b'TALP', padstr(m['talp']))
    if m['tfrq']:
        out += sub(b'TFRQ', struct.pack('>H', m['tfrq']))
    if m['wrapx'] or m['wrapy']:
        out += sub(b'TWRP', struct.pack('>HH', m['wrapx'], m['wrapy']))
    if not is_zero_f(m['aa']):
        out += sub(b'TAAS', invert_x100(m['aa'], 'aa'))
    if not is_zero_f(m['opac']):
        out += sub(b'TOPC', invert_x100(m['opac'], 'opac'))
    if not is_zero_f(m['tfp0']):
        out += sub(b'TFP0', invert_x100(m['tfp0'], 'tfp0'))
    if not is_zero_f(m['tfp1']):
        out += sub(b'TFP1', invert_x100(m['tfp1'], 'tfp1'))
    return out

def build_lwo(d, obj):
    mats = parse_mats(d, obj['mat'][0], obj['nmat'])
    # PNTS
    vraw = d[obj['verts'][0]:obj['verts'][1]]
    pnts = b''.join(vec_le_to_lwo_be(vraw[i:i + 12]) for i in range(0, len(vraw), 12))
    # POLS: FLD faces -> BE u16, surface +1
    fraw = d[obj['faces'][0]:obj['faces'][1]]
    pols = bytearray(); q = 0
    while q < len(fraw):
        fv = struct.unpack_from('<H', fraw, q)[0]; q += 2
        pols += struct.pack('>H', fv)
        for _ in range(fv):
            pols += struct.pack('>H', struct.unpack_from('<H', fraw, q)[0]); q += 2
        srf = struct.unpack_from('<h', fraw, q)[0]; q += 2
        pols += struct.pack('>h', srf + 1)
    # SRFS
    srfs = b''.join(padstr(m['name']) for m in mats)
    body = chunk(b'PNTS', pnts) + chunk(b'SRFS', srfs) + chunk(b'POLS', bytes(pols))
    for m in mats:
        body += chunk(b'SURF', build_surf(m))
    return b'FORM' + struct.pack('>I', 4 + len(body)) + b'LWOB' + body, mats

# ---------------- reader simulator (self-check) ----------------
def simulate(lwo_bytes, legacy=True):
    """Re-implement LWOREAD on lwo_bytes; emit the FLD record bytes
    (materials..faces, exactly as FLDSAVE writes them)."""
    d = lwo_bytes
    assert d[:4] == b'FORM' and d[8:12] == b'LWOB'
    i = 12
    mats_order = []; mats = {}; pnts = b''; faces = []
    def read_str(buf, pos):
        e = buf.index(b'\x00', pos)
        s = buf[pos:e]; pos = e + 1
        if pos & 1: pos += 1
        return s, pos
    while i + 8 <= len(d):
        cid = d[i:i + 4]; ln = struct.unpack('>I', d[i + 4:i + 8])[0]
        pay = d[i + 8:i + 8 + ln]
        if cid == b'PNTS':
            for k in range(0, ln, 12):
                x, y, z = struct.unpack('>3f', pay[k:k + 12])
                pnts += struct.pack('<3f', x, y, z)          # SwapYZ is a no-op
        elif cid == b'SRFS':
            pos = 0
            while pos < ln:
                nm, pos = read_str(pay, pos)
                mats_order.append(nm)
                mats[nm] = {k: (b'' if kd == 's' else
                                (b'\x00' * 3 if kd == 'b3' else
                                 (0 if kd == 'u2' else
                                  (b'\x00' * 4 if kd == 'f' else b'\x00' * 12))))
                            for k, kd in FIELD_ORDER}
                mats[nm]['name'] = nm
        elif cid == b'POLS':
            pos = 0
            while pos < ln:
                fv = struct.unpack_from('>H', pay, pos)[0]; pos += 2
                idx = [struct.unpack_from('>H', pay, pos + 2 * k)[0] for k in range(fv)]
                pos += 2 * fv
                srf = struct.unpack_from('>h', pay, pos)[0]; pos += 2
                faces.append((fv, idx, srf - 1))
        elif cid == b'SURF':
            nm, pos = read_str(pay, 0)
            m = mats[nm]
            while pos + 6 <= ln:
                sid = pay[pos:pos + 4]; sl = struct.unpack('>H', pay[pos + 4:pos + 6])[0]
                sp = pay[pos + 6:pos + 6 + sl]; pos += 6 + sl + (sl & 1)
                if sid == b'COLR': m['color'] = sp[:3]
                elif sid == b'FLAG': m['flags'] = struct.unpack('>H', sp[:2])[0]
                elif sid == b'VLUM':
                    t = struct.unpack('>f', sp[:4])[0]
                    m['lum'] = struct.pack('<f', f32(t * 100.0) if legacy else t)
                elif sid == b'VDIF':
                    t = struct.unpack('>f', sp[:4])[0]; m['diff'] = struct.pack('<f', f32(t * 100.0))
                elif sid == b'VSPC':
                    t = struct.unpack('>f', sp[:4])[0]; m['spec'] = struct.pack('<f', f32(t * 100.0))
                elif sid == b'VRFL':
                    t = struct.unpack('>f', sp[:4])[0]; m['refl'] = struct.pack('<f', f32(t * 100.0))
                elif sid == b'VTRN':
                    t = struct.unpack('>f', sp[:4])[0]; m['trans'] = struct.pack('<f', f32(t * 100.0))
                elif sid == b'GLOS': m['gloss'] = struct.unpack('>H', sp[:2])[0]
                elif sid == b'RFLT': m['refmode'] = struct.unpack('>H', sp[:2])[0]
                elif sid == b'RIMG': m['refimg'] = sp.split(b'\x00')[0]
                elif sid == b'RSAN': m['seam'] = sp[:4][::-1]
                elif sid == b'RIND': m['refidx'] = sp[:4][::-1]
                elif sid == b'EDGE': m['edge'] = sp[:4][::-1]
                elif sid == b'SMAN': m['sman'] = sp[:4][::-1]
                elif sid == b'CTEX': m['ctex'] = sp.split(b'\x00')[0]
                elif sid == b'DTEX': m['dtex'] = sp.split(b'\x00')[0]
                elif sid == b'STEX': m['stex'] = sp.split(b'\x00')[0]
                elif sid == b'RTEX': m['rtex'] = sp.split(b'\x00')[0]
                elif sid == b'TTEX': m['ttex'] = sp.split(b'\x00')[0]
                elif sid == b'BTEX': m['btex'] = sp.split(b'\x00')[0]
                elif sid == b'TIMG': m['timg'] = sp.split(b'\x00')[0]  # DestroyPath: basenames already
                elif sid == b'TFLG': m['tflg'] = struct.unpack('>H', sp[:2])[0]
                elif sid in (b'TSIZ', b'TCTR', b'TFAL', b'TVEL'):
                    x, y, z = sp[0:4], sp[4:8], sp[8:12]
                    key = {b'TSIZ': 'tsiz', b'TCTR': 'tctr', b'TFAL': 'tfal', b'TVEL': 'tvel'}[sid]
                    m[key] = x[::-1] + y[::-1] + z[::-1]     # BE->LE (no swap)
                elif sid == b'TALP': m['talp'] = sp.split(b'\x00')[0]
                elif sid == b'TFRQ': m['tfrq'] = struct.unpack('>H', sp[:2])[0]
                elif sid == b'TWRP':
                    m['wrapx'], m['wrapy'] = struct.unpack('>HH', sp[:4])
                elif sid == b'TAAS':
                    t = struct.unpack('>f', sp[:4])[0]; m['aa'] = struct.pack('<f', f32(t * 100.0))
                elif sid == b'TOPC':
                    t = struct.unpack('>f', sp[:4])[0]; m['opac'] = struct.pack('<f', f32(t * 100.0))
                elif sid == b'TFP0':
                    t = struct.unpack('>f', sp[:4])[0]; m['tfp0'] = struct.pack('<f', f32(t * 100.0))
                elif sid == b'TFP1':
                    t = struct.unpack('>f', sp[:4])[0]; m['tfp1'] = struct.pack('<f', f32(t * 100.0))
        i += 8 + ln + (ln & 1)
    # emit FLD record bytes: materials, nverts+verts, nfaces+faces
    out = bytearray()
    for nm in mats_order:
        m = mats[nm]
        for key, kind in FIELD_ORDER:
            v = m[key]
            if kind == 's': out += v + b'\x00'
            elif kind == 'b3': out += v
            elif kind == 'u2': out += struct.pack('<H', v)
            else: out += v
    out += struct.pack('<I', len(pnts) // 12) + pnts
    out += struct.pack('<I', len(faces))
    for fv, idx, srf in faces:
        out += struct.pack('<H', fv)
        for k in idx: out += struct.pack('<H', k)
        out += struct.pack('<h', srf)
    return bytes(out)

# ---------------- main ----------------
d, objs = walk(FLD)
for name in TARGETS:
    obj = next(o for o in objs if o['name'] == name)
    lwo, mats = build_lwo(d, obj)
    # self-check
    want = (d[obj['mat'][0]:obj['mat'][1]]
            + struct.pack('<I', obj['nv']) + d[obj['verts'][0]:obj['verts'][1]]
            + struct.pack('<I', obj['nf']) + d[obj['faces'][0]:obj['faces'][1]])
    got = simulate(lwo)
    ok = got == want
    print(f'{name}: {obj["nv"]}v {obj["nf"]}f {obj["nmat"]}mats -> {len(lwo)}B LWO ; simulate {"MATCH" if ok else "MISMATCH"}')
    if not ok:
        n = min(len(got), len(want))
        off = next((i for i in range(n) if got[i] != want[i]), n)
        print(f'  first sim-diff at {off}/{n} (lens {len(got)} vs {len(want)})')
        sys.exit(1)
    open(f'/tmp/recovered_{name}', 'wb').write(lwo)
print('all written to /tmp/recovered_*.lwo')
