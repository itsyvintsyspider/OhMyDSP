#!/usr/bin/env python3
"""Read-only decoder for ALSA ASoC topology (.tplg) files as used by SOF (IPC3).

ABI: sof-linux/include/uapi/sound/asoc.h. Token names: include/uapi/sound/sof/tokens.h.
Usage: python3 -I tools/tplg.py <file.tplg> [--tokens <tokens.h>] [widgets|routes|links|all]
Pure parser; no hardware access. Used by tools/gen-ipc3-dmic.py.
"""
import re, struct, sys

MAGIC = 0x41536F43
NAME = 44
T_MIXER, T_BYTES, T_ENUM, T_GRAPH, T_WIDGET, T_DAI_LINK, T_PCM, T_MANIFEST, T_CODEC_LINK, T_BACKEND_LINK, T_PDATA, T_DAI = range(1, 13)
TUPLE_UUID, TUPLE_STRING, TUPLE_BOOL, TUPLE_BYTE, TUPLE_WORD, TUPLE_SHORT = range(6)
TUPLE_NAMES = ["uuid", "string", "bool", "byte", "word", "short"]


def cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def load_tokens(path):
    t = {}
    for line in open(path):
        m = re.match(r"#define\s+(SOF_TKN_\w+)\s+(\d+)", line)
        if m:
            t[int(m.group(2))] = m.group(1)
    return t


def parse_arrays(blob):
    """priv data -> list of (type, [(token, value)]) ; value int, str or bytes(uuid)."""
    out, pos = [], 0
    while pos < len(blob):
        size, typ, n = struct.unpack_from("<III", blob, pos)
        if size < 12 or pos + size > len(blob):
            raise ValueError("bad vendor array at %d (size %d, left %d)" % (pos, size, len(blob) - pos))
        p = pos + 12
        elems = []
        for _ in range(n):
            if typ == TUPLE_UUID:
                tok, = struct.unpack_from("<I", blob, p)
                elems.append((tok, bytes(blob[p + 4:p + 20]))); p += 20
            elif typ == TUPLE_STRING:
                tok, = struct.unpack_from("<I", blob, p)
                elems.append((tok, cstr(blob[p + 4:p + 4 + NAME]))); p += 4 + NAME
            else:
                tok, val = struct.unpack_from("<II", blob, p)
                elems.append((tok, val)); p += 8
        if p != pos + size:
            raise ValueError("vendor array size mismatch at %d: parsed %d, declared %d" % (pos, p - pos, size))
        out.append((typ, elems))
        pos += size
    return out


def blocks(data):
    pos = 0
    while pos < len(data):
        h = struct.unpack_from("<9I", data, pos)
        magic, abi, ver, typ, hsize, vtype, psize, index, count = h
        if magic != MAGIC:
            raise ValueError("bad magic at %d: %#x" % (pos, magic))
        yield dict(type=typ, abi=abi, vendor_type=vtype, index=index, count=count, hsize=hsize,
                   payload=data[pos + hsize:pos + hsize + psize], off=pos)
        pos += hsize + psize


def priv_at(buf, off_of_priv_size):
    n, = struct.unpack_from("<I", buf, off_of_priv_size)
    return n, buf[off_of_priv_size + 4:off_of_priv_size + 4 + n]


def parse_widgets(payload, count):
    ws, pos = [], 0
    for _ in range(count):
        size, wid = struct.unpack_from("<II", payload, pos)
        name = cstr(payload[pos + 8:pos + 8 + NAME]); sname = cstr(payload[pos + 8 + NAME:pos + 8 + 2 * NAME])
        base = pos + 8 + 2 * NAME
        reg, shift, mask, subseq, invert, ign = struct.unpack_from("<6I", payload, base)
        evf, evt, nk = struct.unpack_from("<HHI", payload, base + 24)
        psz_off = base + 32
        assert psz_off + 4 == pos + size, "widget struct size %d does not end at priv.size" % size
        n, pdata = priv_at(payload, psz_off)
        pos += size + n
        kctl = []
        for _k in range(nk):
            hdr_size, ctype = struct.unpack_from("<II", payload, pos)
            cname = cstr(payload[pos + 8:pos + 8 + NAME])
            # ctl_hdr.size covers only the header; the control struct's own `size` follows it
            csize, = struct.unpack_from("<I", payload, pos + hdr_size)
            cn, cdata = priv_at(payload, pos + csize - 4)
            kctl.append(dict(type=ctype, name=cname, priv=cdata, raw=payload[pos:pos + csize + cn]))
            pos += csize + cn
        ws.append(dict(id=wid, name=name, sname=sname, reg=reg, shift=shift, mask=mask, subseq=subseq,
                       invert=invert, ignore_suspend=ign, event_flags=evf, event_type=evt,
                       priv=pdata, kcontrols=kctl))
    assert pos == len(payload), "widget block: %d bytes left" % (len(payload) - pos)
    return ws


def parse_graph(payload, count):
    r = []
    for i in range(count):
        e = payload[i * 3 * NAME:(i + 1) * 3 * NAME]
        r.append(dict(sink=cstr(e[:NAME]), control=cstr(e[NAME:2 * NAME]), source=cstr(e[2 * NAME:])))
    assert len(payload) == count * 3 * NAME
    return r


def parse(path):
    data = open(path, "rb").read()
    topo = dict(widgets=[], routes=[], raw_blocks=[], size=len(data))
    for b in blocks(data):
        topo["raw_blocks"].append((b["type"], b["index"], b["count"], len(b["payload"]), b["vendor_type"]))
        if b["type"] == T_WIDGET:
            for w in parse_widgets(b["payload"], b["count"]):
                w["block_index"] = b["index"]
                topo["widgets"].append(w)
        elif b["type"] == T_GRAPH:
            topo["routes"] += parse_graph(b["payload"], b["count"])
    return topo


def fmt_val(v):
    if isinstance(v, bytes):
        h = v.hex()
        return "uuid:%s-%s-%s-%s-%s" % (h[:8], h[8:12], h[12:16], h[16:20], h[20:])
    return repr(v) if isinstance(v, str) else (str(v) if v < 0x10000 else hex(v))


def main():
    a = sys.argv[1:]
    toks = {}
    if "--tokens" in a:
        i = a.index("--tokens"); toks = load_tokens(a[i + 1]); del a[i:i + 2]
    path, what = a[0], (a[1] if len(a) > 1 else "all")
    topo = parse(path)
    if what in ("blocks", "all"):
        print("file %d bytes; blocks (type,index,count,payload,vendor_type):" % topo["size"])
        for b in topo["raw_blocks"]:
            print("  ", b)
    if what in ("widgets", "all"):
        for w in topo["widgets"]:
            print("WIDGET id=%d name=%r sname=%r blk=%d reg=%#x kctls=%d priv=%dB" % (
                w["id"], w["name"], w["sname"], w["block_index"], w["reg"] & 0xffffffff, len(w["kcontrols"]), len(w["priv"])))
            for typ, el in parse_arrays(w["priv"]):
                print("   [%s]" % TUPLE_NAMES[typ], ", ".join("%s=%s" % (toks.get(t, t), fmt_val(v)) for t, v in el))
            for k in w["kcontrols"]:
                print("   KCTL type=%d %r priv=%dB" % (k["type"], k["name"], len(k["priv"])))
    if what in ("routes", "all"):
        for r in topo["routes"]:
            print("ROUTE %r -> %r%s" % (r["source"], r["sink"], (" ctl=%r" % r["control"]) if r["control"] else ""))


if __name__ == "__main__":
    main()
