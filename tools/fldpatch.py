#!/usr/bin/env python3
"""FLD (Flood scene, v0.113) patcher — editor write-back for scenes WITHOUT
pinned LightWave sources (city/chase/fountain/crash). Greets goes through
lwopatch + lwsread instead; this patches the shipping binary directly.

Layout is a byte-for-byte port of tools/lwsread/FLDSAVE.CPP (the converter
that reproduces GREETS.FLD identically, so the layout is proven against the
engine's reader). The file is fully walked on load; a walk that does not end
EXACTLY at EOF refuses to patch (identity self-test). Patches to fixed-size
fields are in-place; adding a missing light Range envelope splices bytes in
and flips the light's flag bit 16 (the reader is length-driven, so a splice
is structurally safe).

Scales (FLD-file units vs the editor's engine units — see FLD_MAT.CPP):
    diffuse/specular   engine 0..1+  = file 0..100+   (x100 on write)
    luminosity/reflection/transparency/glossiness/baseRGB  direct

API:
    fld = FldFile(path)
    fld.surfaces()                          -> {name: {prop: value}} (engine scale)
    fld.patch_material(name, {prop: val})   -> count of records patched
    fld.lights()                            -> [{i, r,g,b, intensity, range, name}]
    fld.patch_light(i, {r,g,b,intensity,range})
    fld.save(path)                          # identity-checked reserialize

CLI:
    fldpatch.py SCENE.FLD --dump
    fldpatch.py SCENE.FLD --set 'surface|diffuse|0.8' --out patched.fld
"""
import argparse
import math
import struct
import sys

MAT_PROPS = ("baseR", "baseG", "baseB", "diffuse", "specular", "glossiness",
             "luminosity", "transparency", "reflection", "smoothAngle")

SURF_SMOOTHING = 4        # LWREAD.H: Surf_Smoothing bit in the material TFlags
LIGHT_KEYS = ("r", "g", "b", "intensity", "range")

KF_SIZE = 56          # FldKeyFrame: 3 Vectors + 5 floats
ENV_KEY_TAIL = 20     # FrameNumber, LinearValue, Tension, Continuity, Bias


class FldFile:
    def __init__(self, path):
        self.path = path
        self.data = bytearray(open(path, "rb").read())
        self.pos = 0
        # (objName, surfName) -> fixed-block offset (at Color, right after Name)
        self.mats = []          # list of dicts {obj, name, off}
        self.point_lights = []  # editor-index order: dicts, see _walk_light
        self.objects = []       # {name, nmats, nverts, nfaces} — for --dump
        self._walk()
        if self.pos != len(self.data):
            raise ValueError(
                f"{path}: walk ended at {self.pos}, file is {len(self.data)} bytes"
                " — layout mismatch, refusing to patch")

    # ── primitive readers ─────────────────────────────────────────────
    def _u16(self):
        v = struct.unpack_from("<H", self.data, self.pos)[0]
        self.pos += 2
        return v

    def _u32(self):
        v = struct.unpack_from("<I", self.data, self.pos)[0]
        self.pos += 4
        return v

    def _f32(self):
        v = struct.unpack_from("<f", self.data, self.pos)[0]
        self.pos += 4
        return v

    def _cstr(self):
        end = self.data.index(0, self.pos)
        s = self.data[self.pos:end].decode("latin-1")
        self.pos = end + 1
        return s

    def _skip(self, n):
        self.pos += n

    def _envelope(self):
        """Walk an envelope; return (startOffset, keys, channels,
        [value-offsets of Channel[0] per key])."""
        start = self.pos
        keys = self._u32()
        channels = self._u32()
        self._u32()   # EndBehavior
        ch0 = []
        for _ in range(keys):
            ch0.append(self.pos)
            self._skip(4 * channels)
            self._skip(ENV_KEY_TAIL)
        return start, keys, channels, ch0

    # ── structure walk (order = FLDSAVE.CPP) ──────────────────────────
    def _walk(self):
        if self.data[:7] != b"Flood3D":
            raise ValueError(f"{self.path}: not an FLD (missing Flood3D magic)")
        self.pos = 7
        self.version = self._f32()
        self._skip(12)   # FirstFrame, LastFrame, FrameStep
        self._f32()      # FramesPerSecond
        self._envelope()  # AmbientColor
        self._envelope()  # AmbientIntensity
        flags3 = self.data[self.pos:self.pos + 3]
        self.pos += 3
        self.has_obj_names = flags3[0:1] == b"O"
        self.has_mat_names = flags3[1:2] == b"M"
        self.has_lgt_names = flags3[2:3] == b"L"
        if not self.has_mat_names:
            raise ValueError(f"{self.path}: material names not saved — can't patch by name")

        while self.pos < len(self.data):
            chunk = self._u16()
            if chunk == 0x1000:
                self._walk_object()
            elif chunk == 0x2000:
                self._walk_light()
            elif chunk == 0x3000:
                self._walk_camera()
            else:
                raise ValueError(f"{self.path}: unknown chunk 0x{chunk:x} at {self.pos - 2}")

    def _walk_material(self, obj_name):
        name = self._cstr() if self.has_mat_names else ""
        off = self.pos                     # fixed block starts at Color
        self._skip(3)                      # FldColor
        flags_off = self.pos               # TFlags u16 (bit 2 = Surf_Smoothing)
        self._skip(2)                      # Flags u16
        self._skip(4 * 5)                  # Lum, Dif, Spec, Refl, Transp
        self._skip(2 + 2)                  # Glossiness, ReflectionMode
        self._cstr()                       # ReflectionImage (variable length)
        smooth_off = self.pos + 4 * 3      # after SeamAngle, RefrIndex, EdgeTransp
        self._skip(4 * 4)                  # SeamAngle, RefrIndex, EdgeTransp, MaxSmooth
        ctex_start = self.pos              # ColorTexture (projection string)
        self._cstr()
        ctex_end = self.pos                # includes the NUL
        for _ in range(6):                 # Diffuse/Specular/Reflection/
            self._cstr()                   #   Transparency/Bump/TextureImage
        tflg_off = self.pos
        self._skip(2)                      # TextureFlags
        tsiz_off = self.pos
        self._skip(12 * 4)                 # TextureSize/Center/FallOff/Velocity
        self._cstr()                       # TextureAlpha
        self._skip(2 * 3)                  # NoiseFrequencies, WrapX, WrapY
        self._skip(4 * 4)                  # AAStrength, Opacity, TFP0, TFP1
        self.mats.append({"obj": obj_name, "name": name, "off": off,
                          "flags": flags_off, "smooth": smooth_off,
                          "ctex": (ctex_start, ctex_end),
                          "tflg": tflg_off, "tsiz": tsiz_off})

    def _walk_object(self):
        name = self._cstr() if self.has_obj_names else ""
        flags = self._u32()
        nmat = self._u32()
        for _ in range(nmat):
            self._walk_material(name)
        nverts = self._u32()
        self._skip(12 * nverts)
        nfaces = self._u32()
        for _ in range(nfaces):
            fv = self._u16()
            self._skip(2 * fv)
            self._skip(2)                  # Surface s16
        keys = self._u32()
        self._skip(KF_SIZE * keys)
        if flags & 1:
            self._u32()                    # Parent
        if flags & 2:
            self._skip(12)                 # PivotPoint
        if flags & 4:
            self._envelope()               # PolygonSize
        self.objects.append({"name": name, "nmats": nmat,
                             "nverts": nverts, "nfaces": nfaces})

    def _walk_light(self):
        name = self._cstr() if self.has_lgt_names else ""
        flags_off = self.pos
        flags = self._u32()
        keys = self._u32()
        self._skip(KF_SIZE * keys)
        color_off = self.pos
        self._skip(3)                      # FldColor
        _, ikeys, _, intensity_ch0 = self._envelope()
        if flags & 1:
            self._u32()
        if flags & 2:
            self._u32()
        if flags & 4:
            self._envelope()               # Falloff
        if flags & 8:
            self._envelope()               # ConeAngle
        range_ch0 = []
        range_insert_at = self.pos         # where a Range env would go / goes
        if flags & 16:
            _, _, _, range_ch0 = self._envelope()
        # Light_Point = 64: only these become engine omnis; the editor's light
        # index counts them in file order (same mapping as the LWS patcher).
        if flags & 64:
            self.point_lights.append({
                "name": name, "flags_off": flags_off, "flags": flags,
                "color_off": color_off, "intensity_ch0": intensity_ch0,
                "range_ch0": range_ch0, "range_insert_at": range_insert_at,
            })

    def _walk_camera(self):
        flags = self._u32()
        keys = self._u32()
        self._skip(KF_SIZE * keys)
        self._envelope()                   # ZoomFactor
        if flags & 1:
            self._u32()
        if flags & 2:
            self._u32()

    # ── queries ───────────────────────────────────────────────────────
    def surfaces(self):
        out = {}
        for m in self.mats:
            if m["name"] in out:
                continue
            rec = self._read_mat(m["off"])
            rad = struct.unpack_from("<f", self.data, m["smooth"])[0]
            flg = struct.unpack_from("<H", self.data, m["flags"])[0]
            rec["smoothAngle"] = rad * 180.0 / math.pi
            rec["smoothing"] = bool(flg & SURF_SMOOTHING)
            out[m["name"]] = rec
        return out

    def _read_mat(self, off):
        r, g, b = self.data[off], self.data[off + 1], self.data[off + 2]
        lum, dif, spec, refl, tran = struct.unpack_from("<5f", self.data, off + 5)
        gloss = struct.unpack_from("<H", self.data, off + 25)[0]
        return {"baseR": r, "baseG": g, "baseB": b,
                "luminosity": lum, "diffuse": dif * 0.01, "specular": spec * 0.01,
                "reflection": refl, "transparency": tran, "glossiness": gloss}

    def lights(self):
        out = []
        for i, L in enumerate(self.point_lights):
            off = L["color_off"]
            inten = (struct.unpack_from("<f", self.data, L["intensity_ch0"][0])[0]
                     if L["intensity_ch0"] else 0.0)
            rng = (struct.unpack_from("<f", self.data, L["range_ch0"][0])[0]
                   if L["range_ch0"] else 0.0)
            out.append({"i": i, "name": L["name"],
                        "r": self.data[off], "g": self.data[off + 1], "b": self.data[off + 2],
                        "intensity": inten, "range": rng})
        return out

    # ── patches ───────────────────────────────────────────────────────
    def patch_material(self, name, props):
        """props in ENGINE scale (the editor payload). Patches EVERY record
        with that surface name (materials repeat per object) — matches the
        engine's name-keyed edit fan-out."""
        bad = set(props) - set(MAT_PROPS)
        if bad:
            raise ValueError(f"unknown material props {sorted(bad)}")
        count = 0
        for m in self.mats:
            if m["name"] != name:
                continue
            off = m["off"]
            for p, v in props.items():
                if p == "baseR":
                    self.data[off] = max(0, min(255, int(round(float(v)))))
                elif p == "baseG":
                    self.data[off + 1] = max(0, min(255, int(round(float(v)))))
                elif p == "baseB":
                    self.data[off + 2] = max(0, min(255, int(round(float(v)))))
                elif p == "luminosity":
                    struct.pack_into("<f", self.data, off + 5, float(v))
                elif p == "diffuse":
                    struct.pack_into("<f", self.data, off + 9, float(v) * 100.0)
                elif p == "specular":
                    struct.pack_into("<f", self.data, off + 13, float(v) * 100.0)
                elif p == "reflection":
                    struct.pack_into("<f", self.data, off + 17, float(v))
                elif p == "transparency":
                    struct.pack_into("<f", self.data, off + 21, float(v))
                elif p == "glossiness":
                    struct.pack_into("<H", self.data, off + 25,
                                     max(0, min(65535, int(round(float(v))))))
                elif p == "smoothAngle":
                    # Native LWO/FLD field: MaxSmoothingAngle is stored in
                    # RADIANS; the editor/engine speak DEGREES. deg>0 also SETS
                    # the Surf_Smoothing flag (the surface must be flagged smooth
                    # for the engine to honor the angle); deg<=0 CLEARS it so the
                    # surface renders faceted. Clamp to [0,180].
                    deg = max(0.0, min(180.0, float(v)))
                    struct.pack_into("<f", self.data, m["smooth"], deg * math.pi / 180.0)
                    fl = struct.unpack_from("<H", self.data, m["flags"])[0]
                    fl = (fl | SURF_SMOOTHING) if deg > 0.0 else (fl & ~SURF_SMOOTHING)
                    struct.pack_into("<H", self.data, m["flags"], fl)
            count += 1
        return count

    UV_PROJ_NAMES = ["Planar Image Map", "Cylindrical Image Map",
                     "Spherical Image Map", "Cubic Image Map"]

    def patch_material_uv(self, name, proj, sx, sy, sz, axis):
        """Rewrite a surface's UV mapping: ColorTexture projection string
        (length may change → splice + re-walk), TextureFlags axis bits, and
        TextureSize. Patches every record with the surface name; returns the
        count."""
        want = self.UV_PROJ_NAMES[int(proj)].encode("latin-1") + b"\x00"
        count = 0
        for nth in range(len(self.mats)):   # upper bound; re-walk shifts offsets
            recs = [m for m in self.mats if m["name"] == name]
            if nth >= len(recs):
                break
            m = recs[nth]
            struct.pack_into("<H", self.data, m["tflg"],
                             (struct.unpack_from("<H", self.data, m["tflg"])[0] & ~0x7)
                             | (int(axis) & 0x7))
            struct.pack_into("<3f", self.data, m["tsiz"],
                             float(sx), float(sy), float(sz))
            s, e = m["ctex"]
            if self.data[s:e] != want:
                self.data[s:e] = want
                # splice moved everything after `s` — rebuild the index
                self.pos = 0
                self.mats, self.point_lights, self.objects = [], [], []
                self._walk()
                if self.pos != len(self.data):
                    raise ValueError("post-CTEX-splice walk failed — refusing to continue")
            count += 1
        return count

    def patch_light(self, index, props):
        bad = set(props) - set(LIGHT_KEYS)
        if bad:
            raise ValueError(f"unknown light props {sorted(bad)}")
        if index < 0 or index >= len(self.point_lights):
            raise ValueError(f"light {index} out of range (have {len(self.point_lights)})")
        L = self.point_lights[index]
        off = L["color_off"]
        if "r" in props:
            self.data[off] = max(0, min(255, int(round(float(props["r"])))))
        if "g" in props:
            self.data[off + 1] = max(0, min(255, int(round(float(props["g"])))))
        if "b" in props:
            self.data[off + 2] = max(0, min(255, int(round(float(props["b"])))))
        if "intensity" in props:
            for o in L["intensity_ch0"]:
                struct.pack_into("<f", self.data, o, float(props["intensity"]))
        if "range" in props:
            if L["range_ch0"]:
                for o in L["range_ch0"]:
                    struct.pack_into("<f", self.data, o, float(props["range"]))
            else:
                self._insert_range_env(L, float(props["range"]))

    def _insert_range_env(self, L, value):
        """Light has no Range envelope (the FLD-omni-range-0 default) — splice
        in a 1-key 1-channel envelope at the position the reader expects and
        flip the light's flag bit 16. Offsets recorded for OTHER records shift,
        so re-walk afterwards."""
        env = struct.pack("<III", 1, 1, 0)               # Keys, Channels, EndBehavior
        env += struct.pack("<f", value)                  # Channel[0]
        env += struct.pack("<5f", 0.0, value, 0.0, 0.0, 0.0)  # Frame, Linear, T, C, B
        at = L["range_insert_at"]
        flags = struct.unpack_from("<I", self.data, L["flags_off"])[0] | 16
        struct.pack_into("<I", self.data, L["flags_off"], flags)
        self.data[at:at] = env
        # Offsets after `at` are stale — rebuild the index.
        self.pos = 0
        self.mats, self.point_lights, self.objects = [], [], []
        self._walk()
        if self.pos != len(self.data):
            raise ValueError("post-splice walk failed — refusing to continue")

    def save(self, path):
        # Final structural sanity: a full walk must still consume exactly.
        probe = FldFile.__new__(FldFile)
        probe.path = path
        probe.data = self.data
        probe.pos = 0
        probe.mats, probe.point_lights, probe.objects = [], [], []
        probe._walk()
        if probe.pos != len(self.data):
            raise ValueError("patched buffer fails the walk — refusing to write")
        with open(path, "wb") as f:
            f.write(self.data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("fld")
    ap.add_argument("--dump", action="store_true")
    ap.add_argument("--set", action="append", default=[],
                    help="surface|prop|value (engine scale)")
    ap.add_argument("--set-light", action="append", default=[],
                    help="index|key|value")
    ap.add_argument("--out")
    args = ap.parse_args()

    fld = FldFile(args.fld)
    print(f"[fld] {args.fld}: v{fld.version:.3f}, {len(fld.objects)} objects, "
          f"{len(fld.mats)} material records, {len(fld.point_lights)} point lights — walk OK")
    if args.dump:
        for name, props in fld.surfaces().items():
            print(f"  surf {name!r}: " + " ".join(f"{k}={v:.3g}" if isinstance(v, float)
                                                  else f"{k}={v}" for k, v in props.items()))
        for l in fld.lights():
            print(f"  light {l['i']} {l['name']!r}: rgb=({l['r']},{l['g']},{l['b']}) "
                  f"int={l['intensity']:.3g} range={l['range']:.3g}")
    for s in args.set:
        surf, prop, value = s.split("|", 2)
        n = fld.patch_material(surf, {prop: float(value)})
        print(f"  set {surf!r}.{prop} = {value} ({n} records)")
        if n == 0:
            return 1
    for s in args.set_light:
        idx, key, value = s.split("|", 2)
        fld.patch_light(int(idx), {key: float(value)})
        print(f"  set light {idx}.{key} = {value}")
    if args.out:
        fld.save(args.out)
        print(f"[fld] wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
