#!/usr/bin/env python3
"""Difference two AOF3 AO planes (CPU vs GPU) against the SHADING_CONTRACT
tolerance line:  pass <=> |gpu - cpu| <= 0.005*max(|cpu|,|gpu|) + 1e-4.

AOF3 = b"AOF3" + int32 w + int32 h + w*h f32 AO + w*h f32 view-Z (<0 sky)
         + 3*w*h f32 geometric normal.  Written by DEMO --ssao_dump and by
         GpuBench --ssao_dump=PATH.
"""
import struct, sys
import numpy as np


def load(p):
    with open(p, 'rb') as f:
        magic = f.read(4)
        assert magic == b'AOF3', (p, magic)
        w, h = struct.unpack('<ii', f.read(8))
        n = w * h
        ao = np.frombuffer(f.read(4 * n), dtype='<f4').reshape(h, w).astype(np.float64)
        z = np.frombuffer(f.read(4 * n), dtype='<f4').reshape(h, w).astype(np.float64)
        n3 = np.frombuffer(f.read(12 * n), dtype='<f4').reshape(h, w, 3).astype(np.float64)
    return ao, z, n3


def main(cpu_path, gpu_path, label=''):
    ao_c, z_c, n_c = load(cpu_path)
    ao_g, z_g, n_g = load(gpu_path)
    assert ao_c.shape == ao_g.shape, (ao_c.shape, ao_g.shape)
    h, w = ao_c.shape
    n = w * h

    cov_c, cov_g = z_c >= 0, z_g >= 0
    both = cov_c & cov_g
    only_c, only_g = cov_c & ~cov_g, cov_g & ~cov_c

    d = np.abs(ao_g - ao_c)
    tol = 0.005 * np.maximum(np.abs(ao_c), np.abs(ao_g)) + 1e-4
    fail = (d > tol) & both
    nb = int(both.sum())

    print(f'--- {label or cpu_path}  {w}x{h} ---')
    print(f'coverage      : cpu {cov_c.sum()/n*100:6.2f}%   gpu {cov_g.sum()/n*100:6.2f}%   '
          f'both {nb/n*100:6.2f}%   cpu-only {only_c.sum()} px   gpu-only {only_g.sum()} px')
    if nb == 0:
        print('no commonly covered pixels'); return
    dc = d[both]
    print(f'AO mean       : cpu {ao_c[both].mean():.5f}   gpu {ao_g[both].mean():.5f}   '
          f'signed gpu-cpu {(ao_g[both]-ao_c[both]).mean():+.5f}')
    print(f'|d| on both   : mean {dc.mean():.5f}  p50 {np.percentile(dc,50):.5f}  '
          f'p95 {np.percentile(dc,95):.5f}  p99 {np.percentile(dc,99):.5f}  max {dc.max():.5f}')
    print(f'CONTRACT      : fail {int(fail.sum())} / {nb} px = {fail.sum()/nb*100:.3f}%   '
          f'PASS-RATE {100-fail.sum()/nb*100:.3f}%')
    dzr = np.abs(z_g - z_c)[both] / np.maximum(z_c[both], 1e-6)
    nc_ = n_c / np.maximum(np.linalg.norm(n_c, axis=-1, keepdims=True), 1e-12)
    ng_ = n_g / np.maximum(np.linalg.norm(n_g, axis=-1, keepdims=True), 1e-12)
    dn = np.degrees(np.arccos(np.clip(np.abs((nc_ * ng_).sum(-1))[both], -1, 1)))
    print(f'INPUTS        : rel |dZ| mean {dzr.mean():.6f} p95 {np.percentile(dzr,95):.6f} '
          f'max {dzr.max():.5f}   |dN| deg mean {dn.mean():.4f} p95 {np.percentile(dn,95):.4f} '
          f'max {dn.max():.3f}')
    same = both & (np.abs(z_g - z_c) / np.maximum(z_c, 1e-6) < 1e-6)
    if same.sum() > 1000:
        ds = d[same]
        f2 = (d > tol) & same
        print(f'  on the {int(same.sum())} px where the DEPTHS agree to 1e-6 rel: '
              f'mean |d| {ds.mean():.5f}  fail {f2.sum()/max(1,same.sum())*100:.2f}%')
    for thr in (0.01, 0.05, 0.10, 0.25):
        print(f'  |d| > {thr:<5}: {int((dc>thr).sum()):8d} px  {(dc>thr).sum()/nb*100:6.3f}%')
    # where the failures live: occluded vs open
    if fail.sum():
        print(f'  failing px AO: cpu mean {ao_c[fail].mean():.4f}  gpu mean {ao_g[fail].mean():.4f}')
        occ = fail & (ao_c < 0.98)
        print(f'  of the failures, {int(occ.sum())} ({occ.sum()/max(1,fail.sum())*100:.1f}%) '
              f'sit on pixels the CPU calls occluded (ao<0.98)')
    return dict(w=w, h=h, both=nb, fail=int(fail.sum()), mean_abs=float(dc.mean()),
                p95=float(np.percentile(dc, 95)), maxd=float(dc.max()),
                cpu_mean=float(ao_c[both].mean()), gpu_mean=float(ao_g[both].mean()))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else '')
