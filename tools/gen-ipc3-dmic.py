#!/usr/bin/env python3
"""Generate the SOF IPC3 message list Linux sends to build the DMIC capture pipeline (pipeline 10) from sof-hda-generic-2ch.tplg.

Offline only. Mirrors sof-linux/sound/soc/sof: ipc3-topology.c (sof_ipc3_widget_setup_comp_*, sof_link_dmic_load, sof_ipc3_dai_config,
sof_ipc3_route_setup, sof_ipc3_complete_pipeline), ipc3-control.c (sof_ipc3_widget_kcontrol_setup), topology.c (vol_compute_gain),
sof-audio.c (sof_widget_setup_unlocked order). Struct layouts: include/sound/sof/{header,topology,dai,dai-intel,control}.h, uapi/sound/sof/abi.h.

Usage: python3 -I tools/gen-ipc3-dmic.py <tplg> <tokens.h> <out.ipc> [pipeline_name_prefix=PIPELINE.10]
Writes <out.ipc> (blob for the kext) and prints a human-readable dump of every message (review it before sending anything).
Blob: magic 'DIPC' u32, count u32, then per message {u32 len, u32 flags(0), bytes padded to 4}.
Unverified against a real Linux trace: values come from source + topology (see docs/phase3-plan.md).
"""
import struct, sys
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import tplg

# header.h / topology.h constants
GLB_TPLG, GLB_COMP, GLB_DAI = 0x30000000, 0x50000000, 0x80000000
TPLG_COMP_NEW, TPLG_COMP_CONNECT, TPLG_PIPE_NEW, TPLG_PIPE_COMPLETE, TPLG_BUFFER_NEW = 0x00010000, 0x00030000, 0x00100000, 0x00130000, 0x00200000
COMP_SET_VALUE, COMP_SET_DATA, DAI_CONFIG = 0x00010000, 0x00030000, 0x00010000
COMP_HOST, COMP_DAI, COMP_VOLUME, COMP_BUFFER, COMP_EQ_IIR = 1, 2, 5, 12, 13
PROC_EQIIR = 2
FRAME = {"s16le": 0, "s24le": 1, "s32le": 2, "float": 3}          # stream.h SOF_IPC_FRAME_*; s24le -> S24_4LE
DAI_DMIC = 2
STREAM_CAPTURE = 1
CTRL_VOLUME, CTRL_ENUM, CTRL_SWITCH, CTRL_BINARY = 0, 1, 2, 3
CTRL_TYPE_VALUE_CHAN_SET, CTRL_TYPE_DATA_SET = 1, 5
VOLUME_FWL = 16
CTRL_HDR = 92   # sizeof(struct sof_ipc_ctrl_data): rhdr 12 + comp_id/type/cmd/index 16 + host_buffer 28 + num_elems/remaining/msg_index 12 + reserved 24
SOF_ABI_MAGIC = 0x00464F53   # sof_abi_hdr = 32 bytes
# widget ids (SND_SOC_TPLG_DAPM_*)
W_PGA, W_AIF_OUT, W_DAI_OUT, W_BUFFER, W_SCHED, W_EFFECT = 4, 12, 14, 16, 17, 18
# tokens.h numbers used here
TOKEN_NAMES = dict(COMP_UUID="COMP_UUID", SCHED_PERIOD="SCHED_PERIOD", SCHED_PRIORITY="SCHED_PRIORITY", SCHED_MIPS="SCHED_MIPS", SCHED_CORE="SCHED_CORE",
                   SCHED_FRAMES="SCHED_FRAMES", SCHED_TIME_DOMAIN="SCHED_TIME_DOMAIN", BUF_SIZE="BUF_SIZE", BUF_CAPS="BUF_CAPS", BUF_FLAGS="BUF_FLAGS",
                   DAI_TYPE="DAI_TYPE", DAI_INDEX="DAI_INDEX", DAI_DIRECTION="DAI_DIRECTION", PERIOD_SINK="COMP_PERIOD_SINK_COUNT",
                   PERIOD_SOURCE="COMP_PERIOD_SOURCE_COUNT", FORMAT="COMP_FORMAT", CORE_ID="COMP_CORE_ID", RAMP_TYPE="VOLUME_RAMP_STEP_TYPE",
                   RAMP_MS="VOLUME_RAMP_STEP_MS", PROCESS_TYPE="PROCESS_TYPE", DMAC_CONFIG="PCM_DMAC_CONFIG")
T = {}


def cdiv(a, b):  # C signed division (truncate toward zero)
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def u32(x): return x & 0xFFFFFFFF
def s32(x): x &= 0xFFFFFFFF; return x - (1 << 32) if x & 0x80000000 else x


def shift64(i, x):
    x = min(x, 32)
    return u32(i) if x == 0 else u32(((i >> (x - 1)) + 1) >> 1)


def pow32(a, exp, fwl):
    power = 1 << fwl
    if exp == 0: return power
    for _ in range(abs(exp)): power = shift64(power * a, fwl)
    return power if exp > 0 else u32((1 << (fwl << 1)) // power)


def compute_gain(value, tlv_min, tlv_step, tlv_mute):   # topology.c vol_compute_gain
    if value == 0 and tlv_mute: return 0
    db = s32(tlv_min + u32(value * tlv_step) // 100)
    lin = pow32(73533, db, VOLUME_FWL)
    f_step = tlv_step - cdiv(tlv_step, 100)
    if f_step == 50 and (value & 1): lin = shift64(lin * 69419, VOLUME_FWL)
    return lin


def tuples(w):
    d = {}
    for typ, el in tplg.parse_arrays(w["priv"]):
        for tok, v in el: d.setdefault(tok, v)      # first set only (token_instance_num = 1)
    return d


def hdr(size, cmd): return struct.pack("<II", size, cmd)


def comp_base(total, cmd, cid, ctype, pipe, core, ext): return hdr(total, GLB_TPLG | cmd) + struct.pack("<5I", cid, ctype, pipe, core, ext)


def comp_cfg(sink, source, fmt): return hdr(36, 0) + struct.pack("<5I2I", sink, source, 0, fmt, 0, 0, 0)


def with_uuid(body_fn, uuid, ipc_size_wo_uuid):
    ext = 16 if uuid and any(uuid) else 0
    return ext, ipc_size_wo_uuid + ext


def main():
    path, tokh, out = sys.argv[1:4]
    for k, v in TOKEN_NAMES.items(): T[k] = tokens_by_name(tokh, "SOF_TKN_" + v)
    prefix = sys.argv[4] if len(sys.argv) > 4 else "PIPELINE.10"
    topo = tplg.parse(path)
    W = topo["widgets"]
    cid = {w["name"]: i for i, w in enumerate(W)}              # comp_id = widget load order (topology.c:1447, next_comp_id++)
    byname = {w["name"]: w for w in W}
    pw = next(w for w in W if w["id"] == W_SCHED and w["name"].startswith(prefix))
    pipe_idx = pw["block_index"]                               # swidget->pipeline_id = topology block index
    dai_w = next(w for w in W if w["id"] == W_DAI_OUT and w["block_index"] == pipe_idx)
    msgs = []                                                  # (label, bytes)

    def tk(w): return tuples(w)

    def core_of(w): return tk(w).get(T["CORE_ID"], 0)

    def comp_with_uuid(w, ctype, body, cfg_bytes, extra, label_cmd=TPLG_COMP_NEW):
        """body = fixed struct bytes AFTER comp base+config (e.g. direction fields); extra = trailing data before uuid."""
        t = tk(w); uuid = t.get(T["COMP_UUID"], b"\0" * 16)
        ext = 16 if any(uuid) else 0
        total = 28 + 36 + len(body) + len(extra) + ext
        m = comp_base(total, label_cmd, cid[w["name"]], ctype, pipe_idx, core_of(w), ext) + cfg_bytes + body + extra + (uuid if ext else b"")
        assert len(m) == total
        return m

    def cfg_of(w, default_fmt=0):
        t = tk(w)
        return comp_cfg(t.get(T["PERIOD_SINK"], 0), t.get(T["PERIOD_SOURCE"], 0), FRAME[t[T["FORMAT"]]] if T["FORMAT"] in t else default_fmt)

    # 1. PIPE_NEW  (sof_ipc3_widget_setup_comp_pipeline)
    t = tk(pw)
    sched = cid[pw["sname"]]
    m = hdr(48, GLB_TPLG | TPLG_PIPE_NEW) + struct.pack("<10I", cid[pw["name"]], pipe_idx, sched, t.get(T["SCHED_CORE"], 0), t.get(T["SCHED_PERIOD"], 0),
                                                       t.get(T["SCHED_PRIORITY"], 0), t.get(T["SCHED_MIPS"], 0), t.get(T["SCHED_FRAMES"], 0), 0,
                                                       t.get(T["SCHED_TIME_DOMAIN"], 0))
    assert len(m) == 48
    msgs.append(("PIPE_NEW %s (comp %d, sched comp %d)" % (pw["name"], cid[pw["name"]], sched), m))

    # routes of this pipeline, ordered along the capture data path from the DAI
    routes = [r for r in topo["routes"] if r["source"] in cid and r["sink"] in cid and byname[r["source"]]["block_index"] == pipe_idx
              or (r["sink"] in cid and byname.get(r["sink"], {}).get("block_index") == pipe_idx and r["source"] in cid)]
    nxt = {r["source"]: r["sink"] for r in routes}
    chain, cur = [dai_w["name"]], dai_w["name"]
    while cur in nxt and nxt[cur] not in chain:
        cur = nxt[cur]; chain.append(cur)

    # link block for DAI_CONFIG
    link = None
    d = open(path, "rb").read()
    for b in tplg.blocks(d):
        if b["type"] != 10: continue
        p, pos = b["payload"], 0
        for _ in range(b["count"]):
            size, = struct.unpack_from("<I", p, pos)
            name = tplg.cstr(p[pos + 8:pos + 52])
            psz, = struct.unpack_from("<I", p, pos + size - 4)
            nhw, = struct.unpack_from("<I", p, pos + size - 20)
            fmt, = struct.unpack_from("<I", p, pos + 8 + 44 + 44 + 576 + 4 + 8)
            if name == dai_w["sname"]:
                link = dict(fmt=fmt, nhw=nhw, arrays=tplg.parse_arrays(p[pos + size:pos + size + psz]))
            pos += size + psz
    assert link, "no BE link named %r" % dai_w["sname"]
    lk = {}
    for typ, el in link["arrays"]:
        if typ == tplg.TUPLE_SHORT: lk.setdefault("pdm", []).append(dict(el))
        else: lk.update(dict(el))

    for name in chain:
        w = byname[name]
        t = tk(w)
        if w["id"] == W_DAI_OUT:
            # COMP_NEW dai (sof_ipc3_widget_setup_comp_dai) + DAI_CONFIG (sof_link_dmic_load + sof_ipc3_dai_config, flags=HW_PARAMS)
            dtype = {"DMIC": DAI_DMIC}[t[T["DAI_TYPE"]]]
            m = comp_with_uuid(w, COMP_DAI, struct.pack("<4I", t[T["DAI_DIRECTION"]], t[T["DAI_INDEX"]], dtype, 0), cfg_of(w), b"")
            assert len(m) - (16 if any(t.get(T["COMP_UUID"], b"\0")) else 0) == 80
            msgs.append(("COMP_NEW dai %s (comp %d)" % (name, cid[name]), m))
            num_active = lk[tokens_by_name(tokh, "SOF_TKN_INTEL_DMIC_NUM_PDM_ACTIVE")]
            g = lambda n: lk[tokens_by_name(tokh, n)]
            dmic = struct.pack("<I", 0)                                          # sof_ipc_hdr (size left 0 by Linux)
            dmic += struct.pack("<5I", g("SOF_TKN_INTEL_DMIC_DRIVER_VERSION"), g("SOF_TKN_INTEL_DMIC_CLK_MIN"), g("SOF_TKN_INTEL_DMIC_CLK_MAX"),
                                g("SOF_TKN_INTEL_DMIC_SAMPLE_RATE"), 0)          # driver_ipc_version, pdmclk_min/max, fifo_fs, reserved_1
            dmic += struct.pack("<4H", g("SOF_TKN_INTEL_DMIC_FIFO_WORD_LENGTH"), 0,           # fifo_bits; fifo_bits_b stays 0 for FW ABI >= 3.0.1 (ipc3-topology.c:1578)
                                g("SOF_TKN_INTEL_DMIC_DUTY_MIN"), g("SOF_TKN_INTEL_DMIC_DUTY_MAX"))
            dmic += struct.pack("<4I", num_active, 0, 0, g("SOF_TKN_INTEL_DMIC_UNMUTE_RAMP_TIME_MS")) + b"\0" * 20
            pdms = b""
            for i in range(4):
                if i < num_active:
                    pt = lk["pdm"][i]; pg = lambda n: pt[tokens_by_name(tokh, n)]
                    pdms += struct.pack("<I7H3H", 0, pg("SOF_TKN_INTEL_DMIC_PDM_CTRL_ID"), pg("SOF_TKN_INTEL_DMIC_PDM_MIC_A_Enable"),
                                        pg("SOF_TKN_INTEL_DMIC_PDM_MIC_B_Enable"), pg("SOF_TKN_INTEL_DMIC_PDM_POLARITY_A"),
                                        pg("SOF_TKN_INTEL_DMIC_PDM_POLARITY_B"), pg("SOF_TKN_INTEL_DMIC_PDM_CLK_EDGE"), pg("SOF_TKN_INTEL_DMIC_PDM_SKEW"), 0, 0, 0)
                else:
                    pdms += b"\0" * 24
            dmic += pdms
            assert len(dmic) == 164, len(dmic)
            cfg = hdr(216, GLB_DAI | DAI_CONFIG) + struct.pack("<IIHBB", dtype, t[T["DAI_INDEX"]], link["fmt"] & 0xFFFF, 0, 1) + b"\0" * 32 + dmic
            assert len(cfg) == 216
            msgs.append(("DAI_CONFIG dmic index %d (flags=HW_PARAMS)" % t[T["DAI_INDEX"]], cfg))
        elif w["id"] == W_BUFFER:
            m = hdr(44, GLB_TPLG | TPLG_BUFFER_NEW) + struct.pack("<5I", cid[name], COMP_BUFFER, pipe_idx, core_of(w), 0) + struct.pack("<4I", t[T["BUF_SIZE"]], t[T["BUF_CAPS"]], t.get(T["BUF_FLAGS"], 0), 0)
            assert len(m) == 44
            msgs.append(("BUFFER_NEW %s (comp %d) size %d caps %d" % (name, cid[name], t[T["BUF_SIZE"]], t[T["BUF_CAPS"]]), m))
        elif w["id"] == W_EFFECT:
            assert t[T["PROCESS_TYPE"]] == "EQIIR"
            kc = w["kcontrols"][0]; assert kc["type"] == tplg.T_BYTES
            abi = kc["priv"]; magic, = struct.unpack_from("<I", abi, 0); assert magic == SOF_ABI_MAGIC
            dsize, = struct.unpack_from("<I", abi, 8)                  # sof_abi_hdr {magic,type,size,abi,reserved[4]} = 32 bytes (uapi/sound/sof/header.h:31)
            assert dsize + 32 == len(abi), (dsize, len(abi))
            payload = abi[32:]
            body = struct.pack("<II", dsize, PROC_EQIIR) + b"\0" * 28
            m = comp_with_uuid(w, COMP_EQ_IIR, body, cfg_of(w), payload)
            msgs.append(("COMP_NEW effect %s (comp %d) process=EQIIR, %d B coefficient data inline" % (name, cid[name], dsize), m))
            c = struct.pack("<3I", CTRL_HDR + 32 + dsize, GLB_COMP | COMP_SET_DATA, 0) + struct.pack("<4I", cid[name], CTRL_TYPE_DATA_SET, CTRL_BINARY, 0) \
                + b"\0" * 28 + struct.pack("<3I", dsize, 0, 0) + b"\0" * 24 + abi      # sof_ipc_host_buffer is 28 B (stream.h:66)
            assert len(c) == CTRL_HDR + len(abi) == CTRL_HDR + 32 + dsize
            msgs.append(("COMP_SET_DATA (kcontrol setup) %s: EQ coefficients, index 0" % name, c))
        elif w["id"] == W_PGA:
            mixers = [k for k in w["kcontrols"] if k["type"] == tplg.T_MIXER]
            vol = None; ctrls = []
            for idx, k in enumerate(mixers):
                raw = k["raw"]
                nch, = struct.unpack_from("<I", raw, 224)
                mn, mx = struct.unpack_from("<II", raw, 208)
                ctrls.append((idx, k["name"], nch, mn, mx, raw))
            volc = next(c for c in ctrls if c[4] != 1)                     # max != 1 -> volume (max==1 is a switch)
            _, _, nch, mn, mx, raw = volc
            tlv_type, = struct.unpack_from("<I", raw, 72)
            tmin, tstep, tmute = struct.unpack_from("<iII", raw, 76)
            assert tlv_type == 1, "only dB-scale TLV supported (as Linux)"
            tl_min, tl_step, tl_mute = cdiv(tmin, 100), tstep & 0xFFFF, 1 if tmute else 0
            table = [compute_gain(i, tl_min, tl_step, tl_mute) for i in range(mx + 1)]
            body = struct.pack("<5I", nch, table[mn], table[mx], t.get(T["RAMP_TYPE"], 0), t.get(T["RAMP_MS"], 0))
            m = comp_with_uuid(w, COMP_VOLUME, body, cfg_of(w), b"")
            msgs.append(("COMP_NEW volume %s (comp %d) channels %d min %#x max %#x ramp %d initial_ramp %d ms" % (
                name, cid[name], nch, table[mn], table[mx], t.get(T["RAMP_TYPE"], 0), t.get(T["RAMP_MS"], 0)), m))
            for idx, kname, nch2, mn2, mx2, _raw in ctrls:
                if mx2 == 1:
                    chv = b"".join(struct.pack("<II", 0, 0) for _ in range(nch2)); cmd = CTRL_SWITCH      # kzalloc'd: channel 0, value 0 (ipc3-topology.c:1884)
                else:
                    chv = b"".join(struct.pack("<II", i, 1 << VOLUME_FWL) for i in range(nch2)); cmd = CTRL_VOLUME
                c = struct.pack("<3I", CTRL_HDR + len(chv), GLB_COMP | COMP_SET_VALUE, 0) + struct.pack("<4I", cid[name], CTRL_TYPE_VALUE_CHAN_SET, cmd, idx) \
                    + b"\0" * 28 + struct.pack("<3I", nch2, 0, 0) + b"\0" * 24 + chv
                assert len(c) == CTRL_HDR + len(chv)
                msgs.append(("COMP_SET_VALUE (kcontrol setup) %s %r: %s, index %d, %d ch" % (name, kname, "switch" if mx2 == 1 else "volume 0 dB", idx, nch2), c))
        elif w["id"] == W_AIF_OUT:
            m = comp_with_uuid(w, COMP_HOST, struct.pack("<3I", STREAM_CAPTURE, 0, t.get(T["DMAC_CONFIG"], 0)), cfg_of(w), b"")
            assert len(m) - 16 == 76
            msgs.append(("COMP_NEW host %s (comp %d) capture" % (name, cid[name]), m))
        else:
            raise SystemExit("unhandled widget %s id %d" % (name, w["id"]))

    # COMP_CONNECT along the chain, then PIPE_COMPLETE (order of connects is [I]: Linux walks the DAPM list)
    for a, b in zip(chain, chain[1:]):
        msgs.append(("COMP_CONNECT %s(%d) -> %s(%d)" % (a, cid[a], b, cid[b]), hdr(16, GLB_TPLG | TPLG_COMP_CONNECT) + struct.pack("<2I", cid[a], cid[b])))
    msgs.append(("PIPE_COMPLETE %s (comp %d)" % (pw["name"], cid[pw["name"]]), hdr(12, GLB_TPLG | TPLG_PIPE_COMPLETE) + struct.pack("<I", cid[pw["name"]])))

    # blob
    blob = b"DIPC" + struct.pack("<I", len(msgs))
    for _, m in msgs:
        pad = (-len(m)) % 4
        blob += struct.pack("<II", len(m), 0) + m + b"\0" * pad
    open(out, "wb").write(blob)
    for i, (label, m) in enumerate(msgs, 1):
        print("#%02d %-85s %4d B" % (i, label, len(m)))
        for o in range(0, len(m), 32):
            print("      %04x: %s" % (o, " ".join("%08x" % x for x in struct.unpack_from("<%dI" % (min(32, len(m) - o) // 4), m, o))))
    print("\n%d messages, blob %d bytes -> %s" % (len(msgs), len(blob), out))


_tok = {}
def tokens_by_name(tokh, name):
    if not _tok:
        for num, nm in tplg.load_tokens(tokh).items(): _tok[nm] = num
    return _tok[name]


if __name__ == "__main__":
    main()
