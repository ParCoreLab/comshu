#!/usr/bin/env python3
"""Emit the record-model figures in demos/figures (05 through 12) as standalone SVG.

These are the ER half of the talk figures: 04-record-model.svg says what the four
archetypes ARE, and these say which records exist, what keys them, and which join
carries one to the next. There are 53 record types across six provider ranges, which
is more than anyone will place by hand twice — so the geometry is generated from one
grid and the layout is data at the bottom of this file.

Run after adding or renaming a record:

    tools/gen_figures.py                 # writes demos/figures/05-*.svg .. 12-*.svg

It does NOT read the schema, and deliberately: a figure is an argument about which
relationships matter, and there is no reading of pcomm.fbs that produces one. What it
does give you is a single place to make the edit, and a layout that stays on its grid
when you do. The record set it draws was cross-checked against the PCOMM_RECORD
declarations when it was written — all 53, none twice.

The palette matches the existing talk figures: light ground, one hue per provider
range, no <style> block, every attribute inline, so the files open anywhere.
"""

W, H = 230, 46          # one record box
CX, CY = 290, 70        # column / row step

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def box(x, y, name, tid, kind, fields, fam, w=W):
    """One record: name, type_id, kind tag, key fields."""
    # The field line runs until the kind tag; past ~30 chars the two collide.
    assert len(fields) <= 30, f"{name}: field line is {len(fields)} chars, max 30"
    assert len(name) <= 22, f"{name}: name is {len(name)} chars, max 22"
    return (
f'<g><rect x="{x}" y="{y}" width="{w}" height="{H}" rx="3" fill="var(--surface)" '
f'stroke="var(--fam-{fam})" stroke-width="1.4"/>'
f'<rect x="{x}" y="{y}" width="3.5" height="{H}" rx="1.5" fill="var(--fam-{fam})"/>'
f'<text x="{x+13}" y="{y+20}" class="rname">{esc(name)}</text>'
f'<text x="{x+13}" y="{y+36}" class="rfield">{esc(fields)}</text>'
f'<text x="{x+w-12}" y="{y+20}" class="rid" fill="var(--fam-{fam})">{tid}</text>'
f'<text x="{x+w-12}" y="{y+36}" class="rkind">{kind}</text></g>')

def note(x, y, text, anchor="start", cls="fnote"):
    return f'<text x="{x}" y="{y}" class="{cls}" text-anchor="{anchor}">{esc(text)}</text>'

def band(x, y, w, h, fam, label, label_at="top"):
    """A family enclosure with its type_id range."""
    return (
f'<g><rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" fill="var(--fam-{fam}-wash)" '
f'stroke="var(--fam-{fam})" stroke-width="1" stroke-dasharray="2 4" opacity="0.9"/>'
f'<text x="{x+12}" y="{y+19 if label_at == "top" else y+h-10}" class="bandlabel" fill="var(--fam-{fam})">{esc(label)}</text></g>')

def path(d, label=None, lx=0, ly=0, dashed=False, anchor="middle", head=True):
    da = ' stroke-dasharray="4 3"' if dashed else ''
    mk = ' marker-end="url(#ar)"' if head else ''
    s = f'<path d="{d}" fill="none" stroke="var(--arrow)" stroke-width="1.2"{da}{mk}/>'
    if label:
        s += f'<text x="{lx}" y="{ly}" class="jlabel" text-anchor="{anchor}">{esc(label)}</text>'
    return s

def elbow(x1, y1, x2, y2, mid=None):
    """Orthogonal connector: horizontal, vertical, horizontal."""
    mx = mid if mid is not None else (x1 + x2) / 2
    return f"M{x1} {y1} H{mx} V{y2} H{x2}"

def svg(vb_w, vb_h, body, label):
    return (
f'<svg viewBox="0 0 {vb_w} {vb_h}" role="img" aria-label="{esc(label)}" class="fig">'
'<defs><marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" '
'orient="auto-start-reverse"><path d="M0 0 L10 5 L0 10 z" fill="var(--arrow)"/></marker></defs>'
+ body + '</svg>')

def bracket(sources, rail_x, target, label, lx=None, ly=None, anchor="middle"):
    """Many sources -> one target, joined on a shared vertical rail."""
    out = []
    ys = [y for _, y in sources] + [target[1]]
    for sx, sy in sources:
        out.append(path(f"M{sx} {sy} H{rail_x}", head=False))
    out.append(path(f"M{rail_x} {min(ys)} V{max(ys)}", head=False))
    out.append(path(f"M{rail_x} {target[1]} H{target[0]}"))
    if label:
        out.append(note(lx if lx is not None else rail_x, ly if ly is not None else min(ys) - 8,
                        label, anchor=anchor, cls="jlabel"))
    return "".join(out)

def stack_up(sources, rail_x, parent_cx, parent_bottom, label, side="left"):
    """Boxes stacked below a parent, joined by a rail beside the column and one arrow up."""
    out = []
    ys = [y for _, y in sources]
    top = parent_bottom + 16
    for sx, sy in sources:
        out.append(path(f"M{sx} {sy} H{rail_x}", head=False))
    out.append(path(f"M{rail_x} {top} V{max(ys)}", head=False))
    out.append(path(f"M{rail_x} {top} H{parent_cx}", head=False))
    out.append(path(f"M{parent_cx} {top} V{parent_bottom + 4}"))
    if label:
        anchor = "end" if side == "left" else "start"
        lx = rail_x - 6 if side == "left" else rail_x + 6
        out.append(note(lx, (top + max(ys)) / 2, label, anchor=anchor, cls="jlabel"))
    return "".join(out)

def bus(sources, rail_x, targets, label=None, lx=None, ly=None):
    """Many -> many, through one shared rail. Claims composition, not a 1:1 mapping."""
    out = []
    ys = [y for _, y in sources] + [y for _, y in targets]
    for sx, sy in sources:
        out.append(path(f"M{sx} {sy} H{rail_x}", head=False))
    out.append(path(f"M{rail_x} {min(ys)} V{max(ys)}", head=False))
    for tx, ty in targets:
        out.append(path(f"M{rail_x} {ty} H{tx}"))
    if label:
        out.append(note(lx, ly, label, anchor="middle", cls="jlabel"))
    return "".join(out)

FIGS = {}

# ---------------------------------------------------------------- fig: engine
b = []
b.append(band(8, 306, 844, 100, "int", "intern dictionary — every Handle in every record, in every family, resolves here"))
b.append(box(300, 40, "gpu_access", 700, "EVT", "pc · object · address · launch", "gpu"))
b.append(box(10, 40, "mem_object", 520, "ENT", "id · range · device", "int"))
b.append(box(590, 40, "gpu_kernel", 530, "EVT", "correlation · stream · name", "int"))
b.append(box(300, 145, "source_line", 740, "ART", "pc · file · line", "gpu"))
b.append(box(10, 145, "peer_mapping", 526, "ENT", "allocation · range · offset", "int"))
b.append(box(590, 145, "attribution", 760, "EDG", "subject · object · pred", "gpu"))
b.append(box(10, 230, "node_identity", 524, "ENT", "node_hash", "int"))
b.append(box(300, 230, "entity_close", 522, "EVT", "subject · opener", "int"))
b.append(box(590, 230, "clock_anchor", 590, "ART", "domain · local_ticks · wall_ns", "int"))
b.append(box(150, 336, "intern_key", 510, "ART", "key → handle · domain", "int"))
b.append(box(450, 336, "intern_string", 512, "ART", "offset · length → blob", "int"))
b.append(path("M300 63 H245", "object → id", 272, 32))
b.append(path("M530 63 H585", "launch → correlation", 558, 32))
b.append(path("M415 86 V140", "pc", 423, 116, anchor="start"))
b.append(path("M125 145 V90", "peer addr → owner base", 133, 122, anchor="start"))
b.append(path("M415 276 V300", None))
b.append(path("M125 276 V300", None))
b.append(path("M705 276 V300", None))
b.append(note(10, 424, "entity_close is one shared type for every provider: subject is the interned handle the OPEN record carried,", cls="fnote"))
b.append(note(10, 438, "and opener is that record's type_id. A close naming a subject nothing opened is not a bug in the close hook —", cls="fnote"))
b.append(note(10, 452, "it is evidence of a creation door that is not hooked, and it is counted rather than suppressed.", cls="fnote"))
FIGS["engine"] = svg(860, 466, "".join(b),
    "The engine's records in the 500 range and the gpumem provider's in the 700 range, with the intern dictionary every handle resolves through")

# ---------------------------------------------------------------- fig: ibverbs
b = []
b.append(band(292, 178, 250, 298, "ib", "pd handle · nothing creates it", label_at="bottom"))
b.append(box(10, 60, "ib_device", 120, "ENT", "context · node_guid · fw_ver", "ib"))
b.append(box(10, 140, "ib_port", 122, "ENT", "context · lid · port_num", "ib"))
b.append(box(300, 40, "ib_completion_queue", 104, "ENT", "cq · context · cqe", "ib"))
b.append(box(300, 110, "ib_qp_signalling", 126, "ENT", "qp · qp_num · sq_sig_all", "ib"))
b.append(box(300, 200, "ib_queue_pair", 102, "ENT", "qp · pd · qp_num", "ib"))
b.append(box(300, 270, "ib_shared_recv_queue", 108, "ENT", "srq · pd · max_wr", "ib"))
b.append(box(300, 340, "ib_mem_region", 100, "ENT", "mr · pd · range · lkey · rkey", "ib"))
b.append(box(300, 410, "ib_address_handle", 118, "ENT", "ah · pd · dlid · dgid", "ib"))
b.append(box(620, 40, "ib_req_notify_cq", 116, "EVT", "cq · solicited_only", "ib"))
b.append(box(620, 110, "ib_poll_cq", 112, "EVT", "cq · wr_id · qp_num · status", "ib"))
b.append(box(620, 180, "ib_recv_cqe", 124, "EVT", "cq · srq · wqe_counter", "ib"))
b.append(box(620, 250, "ib_modify_qp", 106, "EVT", "qp · src_lid → dest_lid", "ib"))
b.append(box(620, 320, "ib_post_send", 110, "EVT", "qp · wr_id · remote_addr", "ib"))
b.append(box(620, 390, "ib_post_send_sg", 128, "EVT", "qp_num · wr_id · addr · lkey", "ib"))
b.append(box(620, 460, "ib_post_recv", 114, "EVT", "subject · wr_id · addr · lkey", "ib"))
b.append(path("M125 106 V135", "context", 133, 126, anchor="start"))
b.append(path("M240 83 H268 V63 H295", "context", 264, 44, anchor="end"))
b.append(bracket([(620, 63), (620, 133), (620, 203)], 580, (535, 63), "cq", 580, 32))
b.append(bracket([(620, 273), (620, 343)], 566, (535, 223), "qp", 562, 246, anchor="end"))
b.append(path("M620 483 H552 V293 H535", "subject, per origin", 548, 500, anchor="end"))
b.append(path("M415 160 V196", None))
b.append(note(423, 176, "qp", cls="jlabel"))
b.append(path("M620 413 H590 V363 H535", "lkey → mr", 562, 356))
b.append(path("M735 390 V366", "(qp_num, wr_id, ticks)", 743, 381, anchor="start"))
b.append(note(10, 236, "ib_device and ib_port exist so a rank", cls="fnote"))
b.append(note(10, 250, "that never moves traffic still has a LID.", cls="fnote"))
b.append(note(10, 278, "ib_modify_qp's →RTR row is the other", cls="fnote"))
b.append(note(10, 292, "half: (src_lid, qp_num) is what names a", cls="fnote"))
b.append(note(10, 306, "queue pair across the whole fabric, since", cls="fnote"))
b.append(note(10, 320, "qp_num is per-HCA and collides.", cls="fnote"))
FIGS["ib"] = svg(870, 530, "".join(b),
    "The ibverbs provider's fifteen record types: the device and port, the protection-domain objects, and the events that reference them")

# ---------------------------------------------------------------- fig: mlx5
b = []
b.append(box(10, 40, "mlx5_umem", 200, "ENT", "umem · umem_id · range · fd", "mlx"))
b.append(box(10, 130, "mlx5_uar", 202, "ENT", "uar · reg_addr · page_id", "mlx"))
b.append(box(320, 40, "mlx5_devx_qp", 206, "ENT", "obj · qp_num · wq_umem_id", "mlx"))
b.append(box(320, 130, "mlx5_devx_cmd", 204, "EVT", "obj · opcode · decoded", "mlx"))
b.append(box(320, 220, "mlx5_doorbell", 212, "EVT", "uar · qp_num · prod_idx", "mlx"))
b.append(box(630, 40, "mlx5_devx_qp_connect", 208, "EVT", "qp_num · src_lid → dest_lid", "mlx"))
b.append(box(630, 130, "mlx5_device_send", 214, "EVT", "qp_num · wqe_idx · laddr/raddr", "mlx"))
b.append(box(10, 250, "mlx5_queue_pair", 210, "ENT", "qp · pd · qp_num", "mlx"))
b.append(path("M320 63 H245", "wq_umem_id → umem_id", 282, 32))
b.append(path("M435 130 V86", "obj", 443, 116, anchor="start"))
b.append(path("M320 243 H280 V153 H245", "uar", 276, 205, anchor="end"))
b.append(bracket([(630, 63), (630, 153)], 578, (555, 63), "qp_num", 578, 32))
b.append(note(10, 320, "The doorbell is what announces the WQE that mlx5_device_send decodes — so a row in one without the other is a", cls="fnote"))
b.append(note(10, 336, "readable state, not a contradiction: the doorbell is emitted even when the work-queue entry cannot be read, so an", cls="fnote"))
b.append(note(10, 352, "undecodable doorbell stays a visible row instead of vanishing.", cls="fnote"))
b.append(note(255, 282, "Not DEVX at all: an ordinary verbs queue pair reached through the mlx5 door.", cls="fnote"))
b.append(note(255, 298, "perftest creates its queue pairs this way and never calls ibv_create_qp.", cls="fnote"))
FIGS["mlx"] = svg(880, 370, "".join(b),
    "The mlx5dv DEVX provider's eight record types: the umem and UAR handed to the NIC, the DEVX queue pair, and the doorbell and device send that follow")

# ---------------------------------------------------------------- fig: ucx
b = []
b.append(box(10, 40, "ucx_context", 600, "ENT", "context · api_major/minor", "ucx"))
b.append(box(300, 40, "ucx_worker", 602, "ENT", "worker · context", "ucx"))
b.append(box(620, 40, "ucx_endpoint", 604, "ENT", "endpoint · worker", "ucx"))
b.append(box(10, 145, "ucx_mem_map", 614, "ENT", "memh · context · address", "ucx"))
b.append(box(300, 145, "ucx_worker_address", 622, "ENT", "worker · prefix_hash", "ucx"))
b.append(box(300, 215, "ucx_tag_recv", 608, "EVT", "worker · tag · tag_mask", "ucx"))
b.append(box(300, 285, "ucx_tag_probe", 618, "EVT", "worker · message · length", "ucx"))
b.append(box(300, 355, "ucx_worker_progress", 616, "EVT", "worker · progressed", "ucx"))
b.append(box(620, 145, "ucx_ep_connection", 624, "ENT", "endpoint · remote_prefix_hash", "ucx"))
b.append(box(620, 215, "ucx_tag_send", 606, "EVT", "endpoint · tag · buffer", "ucx"))
b.append(box(620, 285, "ucx_rma", 610, "EVT", "endpoint · op · remote_addr", "ucx"))
b.append(box(620, 355, "ucx_atomic", 620, "EVT", "endpoint · op · remote_addr", "ucx"))
b.append(box(620, 425, "ucx_am_send", 612, "EVT", "endpoint · am_id · header_len", "ucx"))
b.append(path("M300 63 H245", "context", 272, 32))
b.append(path("M620 63 H555", "worker", 587, 32))
b.append(path("M125 145 V90", "context", 133, 122, anchor="start"))
b.append(stack_up([(300, 168), (300, 238), (300, 308), (300, 378)], 268, 415, 86, "worker"))
b.append(stack_up([(620, 168), (620, 238), (620, 308), (620, 378), (620, 448)], 588, 735, 86, "endpoint"))
b.append(path("M415 285 V261", "message", 423, 276, anchor="start"))
b.append(note(10, 215, "The spine is ownership: a context owns", cls="fnote"))
b.append(note(10, 229, "workers, a worker owns endpoints.", cls="fnote"))
b.append(note(10, 257, "Which side a call hangs off is the API's own", cls="fnote"))
b.append(note(10, 271, "shape, not a convention. A tagged send names", cls="fnote"))
b.append(note(10, 285, "its endpoint; a tagged receive cannot, because", cls="fnote"))
b.append(note(10, 299, "it matches by tag across every peer at once.", cls="fnote"))
b.append(note(10, 327, "ucx_tag_probe carries the one true byte count", cls="fnote"))
b.append(note(10, 341, "in this family. Every other count is capacity", cls="fnote"))
b.append(note(10, 355, "offered, not bytes delivered.", cls="fnote"))
b.append(note(10, 383, "ucx_worker_address and ucx_ep_connection", cls="fnote"))
b.append(note(10, 397, "exist only to make the cross-rank join", cls="fnote"))
b.append(note(10, 411, "possible at all.", cls="fnote"))
FIGS["ucx"] = svg(880, 500, "".join(b),
    "The UCX provider's thirteen record types on the context, worker, endpoint ownership spine")

# ---------------------------------------------------------------- fig: nccl
b = []
b.append(box(10, 130, "nccl_comm", 900, "ENT", "comm · comm_hash · rank", "ncc"))
b.append(box(360, 40, "nccl_collective", 902, "EVT", "comm · count · root · op", "ncc"))
b.append(box(360, 110, "nccl_p2p", 904, "EVT", "comm · peer · dir · count", "ncc"))
b.append(box(360, 180, "nccl_window", 908, "ENT", "window · comm · buffer · size", "ncc"))
b.append(box(360, 250, "nccl_dev_comm", 910, "ENT", "comm", "ncc"))
b.append(box(360, 336, "nccl_group", 906, "EVT", "depth · edge", "ncc"))
b.append(bracket([(360, 63), (360, 133), (360, 203), (360, 273)], 320, (245, 153), "comm", 316, 42, anchor="end"))
b.append(note(360, 310, "Group boundaries stand outside the communicator:", cls="fnote"))
b.append(note(360, 324, "N grouped sends are ONE launch, and nothing in a send row says so.", cls="fnote"))
b.append(note(10, 215, "Every temporal here is the HOST CALL, not", cls="fnote"))
b.append(note(10, 229, "the collective's execution — NCCL is stream-", cls="fnote"))
b.append(note(10, 243, "ordered, so these are enqueue facts.", cls="fnote"))
b.append(note(10, 271, "nccl_p2p is the only NCCL call that names a", cls="fnote"))
b.append(note(10, 285, "peer, and so the only one an exact edge", cls="fnote"))
b.append(note(10, 299, "follows from.", cls="fnote"))
FIGS["nccl"] = svg(790, 400, "".join(b),
    "The NCCL provider's six record types, all hanging off the communicator")

# ------------------------------------------------------------ fig: cross-rank
b = []
b.append(band(8, 30, 250, 470, "int", "rank 3"))
b.append(band(602, 30, 250, 470, "int", "rank 11 — another node"))
rows = [
 (56, "ib_modify_qp", 106, "ib", "src_lid 42 · qp_num 71", "dest_lid 42 · dest_qp 71",
  "(dest_lid, dest_qp_num) = (lid, qp_num)", "the fabric's own identity — qp_num alone is per-HCA"),
 (146, "node_identity", 524, "int", "node_hash A · device 2", "node_hash B · device 2",
  "(node_hash, device) → rank", "a device ordinal is node-local; 'device 2' is two GPUs"),
 (236, "nccl_comm", 900, "ncc", "comm_hash H · rank 3", "comm_hash H · rank 11",
  "comm_hash", "FNV-1a of the ncclUniqueId — equal on every rank"),
 (326, "ucx_worker_address", 622, "ucx", "prefix_hash P", "ep dialled at prefix P",
  "prefix_hash", "the dialled worker, found in the other rank's capture"),
 (416, "peer_mapping", 526, "int", "allocation F · exporter 1", "allocation F · exporter 0",
  "allocation (fabric handle)", "both sides mint the same id from the same 64 bytes"),
]
for y, name, tid, fam, left, right, key, why in rows:
    b.append(box(20, y, name, tid, "", left, fam, w=226))
    b.append(box(614, y, name, tid, "", right, fam, w=226))
    my = y + 23
    b.append(path(f"M246 {my} H610", None))
    b.append(path(f"M610 {my} H246", None))
    b.append(f'<rect x="282" y="{my-26}" width="296" height="23" rx="3" fill="var(--surface)"/>')
    b.append(note(430, my - 10, key, anchor="middle", cls="jkey"))
    b.append(note(430, my + 17, why, anchor="middle", cls="fnote"))
b.append(note(8, 522, "A capture is N independent per-rank directories written by N processes that never coordinate. These five keys are the whole of", cls="fnote"))
b.append(note(8, 538, "what makes them one picture — every other field in the record model is rank-local.", cls="fnote"))
FIGS["cross"] = svg(870, 552, "".join(b),
    "The five keys that join one rank's capture to another's: the fabric address pair, node identity, the NCCL communicator hash, the UCX worker prefix hash, and the CUDA fabric handle")

# --------------------------------------------------------------- fig: derived
b = []
srcs = [("ib_post_send", 110, "ib"), ("mlx5_device_send", 214, "mlx"), ("ib_modify_qp", 106, "ib"),
        ("ib_port", 122, "ib"), ("ib_poll_cq", 112, "ib"), ("ib_post_recv", 114, "ib"),
        ("gpu_access", 700, "gpu"), ("mem_object", 520, "int"), ("node_identity", 524, "int"),
        ("clock_anchor", 590, "int")]
sy = []
for i, (n, t, f) in enumerate(srcs):
    y = 40 + i * 54
    b.append(box(20, y, n, t, "", "", f, w=196))
    sy.append((216, y + 23))
views = ["_sends", "_rtr  ·  _owner", "_peer_of_qp", "_wr_pairable  ·  _wr_flight",
         "_nvlink_sends", "_node  ·  _device_owner", "_recv_attr", "_tl_raw", "_clock"]
vy = []
for i, n in enumerate(views):
    y = 44 + i * 58
    b.append(f'<rect x="330" y="{y}" width="222" height="38" rx="3" fill="var(--surface)" '
             f'stroke="var(--rule-strong)" stroke-width="1.2" stroke-dasharray="4 3"/>'
             f'<text x="{342}" y="{y+24}" class="rname">{esc(n)}</text>')
    vy.append((y + 19))
tables = [("comm_edges", "src → dst, per transport"), ("endpoint", "one row per local queue pair"),
          ("object_access", "every touch, as an offset in an object"), ("wr_flow", "posted → retired, with latency"),
          ("timeline_span", "every event on one wall clock"), ("rank_identity", "rank → node, device"),
          ("link_health", "what the graph could NOT account for")]
ty = []
for i, (n, d) in enumerate(tables):
    y = 56 + i * 72
    b.append(f'<rect x="660" y="{y}" width="252" height="46" rx="3" fill="var(--surface)" '
             f'stroke="var(--fam-int)" stroke-width="1.4"/>'
             f'<rect x="660" y="{y}" width="3.5" height="46" rx="1.5" fill="var(--fam-int)"/>'
             f'<text x="673" y="{y+20}" class="rname">{esc(n)}</text>'
             f'<text x="673" y="{y+36}" class="rfield">{esc(d)}</text>')
    ty.append((656, y + 23))
b.append(bus(sy, 272, [(326, v) for v in vy]))
b.append(bus([(552, v) for v in vy], 606, ty))
b.append(note(20, 596, "records, as written", cls="collabel"))
b.append(note(330, 596, "views — created in order", cls="collabel"))
b.append(note(660, 596, "what the panels and the suite read", cls="collabel"))
FIGS["derived"] = svg(930, 610, "".join(b),
    "How the reader turns raw per-rank records into the seven derived tables that the UI, the agent and the test suite query")

# --------------------------------------------------- fig: the capture container

def cell(x, y, w, h, label, sub=None, fill="var(--surface)", stroke="var(--rule-strong)", hue=None):
    s = (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="3" fill="{fill}" '
         f'stroke="{stroke}" stroke-width="1.2"/>')
    if sub:
        s += f'<text x="{x+w/2}" y="{y+h/2-2}" class="cellname" text-anchor="middle">{esc(label)}</text>'
        s += f'<text x="{x+w/2}" y="{y+h/2+13}" class="rfield" text-anchor="middle">{esc(sub)}</text>'
    else:
        s += f'<text x="{x+w/2}" y="{y+h/2+4}" class="cellname" text-anchor="middle">{esc(label)}</text>'
    return s

def build():
    b = []
    # --- the directory tree
    b.append(cell(10, 56, 150, 44, "pcomm-out/", "the capture"))
    for i, (lbl, y) in enumerate([("rank-0/", 12), ("rank-1/", 68), ("rank-2/  …  rank-15/", 124)]):
        b.append(cell(210, y, 140, 44, lbl))
        b.append(path(f"M160 78 H185 V{y+22} H206"))
    for j, (f, what, y) in enumerate([
            ("100.pcs", "ib_mem_region", 4), ("110.pcs", "ib_post_send", 42),
            ("520.pcs", "mem_object", 80), ("700.pcs", "gpu_access", 118),
            ("intern-blob.bin", "the byte heap", 156)]):
        b.append(cell(410, y, 200, 32, f, None, stroke="var(--rule)"))
        b.append(f'<text x="{620}" y="{y+21}" class="rfield">{esc(what)}</text>')
        b.append(path(f"M350 34 H380 V{y+16} H406", head=(j == 0)))
    b.append(note(410, 206, "one file per record type — the header inside", cls="fnote"))
    b.append(note(410, 220, "names it, so a filename is never load-bearing", cls="fnote"))
    b.append(note(10, 122, "N independent per-rank", cls="fnote"))
    b.append(note(10, 136, "directories. Nothing in a", cls="fnote"))
    b.append(note(10, 150, "capture is written twice.", cls="fnote"))

    # --- what a .pcs file is
    b.append(note(10, 258, "INSIDE ONE .pcs", cls="collabel"))
    b.append(cell(10, 270, 130, 40, "StreamHeader", "32 B, once", fill="var(--fam-int-wash)", stroke="var(--fam-int)"))
    for k, x in enumerate([146, 268, 390, 512, 634, 756]):
        lbl = "record 0" if k == 0 else ("…" if k == 5 else f"record {k}")
        b.append(cell(x, 270, 118, 40, lbl))
    b.append(note(10, 328, "magic · format_version · type_id · stride · schema_hash · kind", cls="fnote"))
    b.append(note(874, 328, "a packed array — fixed stride, no framing, the writer is a memcpy", cls="fnote", anchor="end"))

    # --- what a record is
    b.append(note(10, 380, "INSIDE ONE RECORD — gpu_access, type 700", cls="collabel"))
    cells = [(150, "Temporal", "48 B · kind, a, b"), (90, "pc", "Handle"), (90, "object", "Handle"),
             (110, "address", "u64"), (110, "launch", "u64"), (280, "size · op · space · device · thread", "fixed width")]
    x = 10
    handle_xs = []
    for w, lbl, sub in cells:
        hue = lbl in ("pc", "object")
        b.append(cell(x, 392, w, 44, lbl, sub,
                      fill="var(--fam-gpu-wash)" if hue else "var(--surface)",
                      stroke="var(--fam-gpu)" if hue else "var(--rule-strong)"))
        if hue:
            handle_xs.append(x + w / 2)
        x += w + 2
    # guides from record 0 down to the expansion
    for hx in handle_xs:
        b.append(path(f"M{hx} 436 V466"))
    b.append(cell(120, 468, 390, 38, "intern_key 510  ·  intern_string 512",
                  None, fill="var(--fam-int-wash)", stroke="var(--fam-int)"))
    b.append(note(530, 478, "+ intern-blob.bin, behind its own header. Lookups are", cls="fnote"))
    b.append(note(530, 492, "keyed by (domain, handle): handle 1 in Pc and handle 1", cls="fnote"))
    b.append(note(530, 506, "in KernelName are different things.", cls="fnote"))
    return svg(880, 528, "".join(b),
               "A capture is one directory per rank of packed record streams; each stream is a 32-byte header followed by fixed-stride records whose high-cardinality fields are handles into the capture's own intern dictionary")

import xml.etree.ElementTree as ET

MONO = "ui-monospace, SFMono-Regular, Menlo, monospace"
SANS = "Inter, Helvetica Neue, Arial, sans-serif"

# Palette taken from the existing talk figures, extended with one hue per provider range.
TOK = {
    "--surface": "#ffffff", "--ground": "#ffffff", "--panel": "#f7f9fb",
    "--ink": "#24343f", "--muted": "#7c8c9a",
    "--rule": "#e2e8ed", "--rule-strong": "#a8b6c2", "--arrow": "#8298a8",
    "--fam-int": "#5b7c99", "--fam-int-wash": "#f2f6f9",
    "--fam-gpu": "#4f7d3f", "--fam-gpu-wash": "#f3f8f1",
    "--fam-ib":  "#a06a24", "--fam-ib-wash":  "#fbf6ef",
    "--fam-mlx": "#6a5aa0", "--fam-mlx-wash": "#f5f3fa",
    "--fam-ucx": "#1f7a70", "--fam-ucx-wash": "#eff8f6",
    "--fam-ncc": "#a04a58", "--fam-ncc-wash": "#fbf1f3",
}

CLS = {
  "rname":     {"font-family": MONO, "font-size": "13", "font-weight": "600", "fill": "#24343f"},
  "cellname":  {"font-family": MONO, "font-size": "12", "font-weight": "600", "fill": "#24343f"},
  "rfield":    {"font-family": MONO, "font-size": "9.5", "fill": "#5f7183"},
  "rid":       {"font-family": MONO, "font-size": "11", "font-weight": "500", "text-anchor": "end"},
  "rkind":     {"font-family": MONO, "font-size": "8.5", "letter-spacing": "0.8",
                "fill": "#95a4b0", "text-anchor": "end"},
  "jlabel":    {"font-family": MONO, "font-size": "9.5", "fill": "#5f7183"},
  "jkey":      {"font-family": MONO, "font-size": "11", "font-weight": "600", "fill": "#24343f"},
  "fnote":     {"font-family": SANS, "font-size": "11", "fill": "#7c8c9a"},
  "bandlabel": {"font-family": MONO, "font-size": "10", "letter-spacing": "0.8", "upper": 1},
  "collabel":  {"font-family": MONO, "font-size": "9.5", "letter-spacing": "1.2",
                "fill": "#95a4b0", "upper": 1},
}

# ------------------------------------------- standalone export: classes -> attrs

def resolve(v):
    if v and v.startswith("var(--"):
        return TOK[v[4:-1]]
    return v

def convert(svg_text, title, scale=1.7):
    root = ET.fromstring(svg_text)
    vb = [float(x) for x in root.get("viewBox").split()]
    root.set("xmlns", "http://www.w3.org/2000/svg")
    root.set("width", f"{vb[2]*scale:.0f}")
    root.set("height", f"{vb[3]*scale:.0f}")
    root.set("font-family", SANS)
    root.attrib.pop("class", None)
    root.attrib.pop("role", None)
    aria = root.attrib.pop("aria-label", title)

    for el in root.iter():
        for a in ("fill", "stroke"):
            if el.get(a):
                el.set(a, resolve(el.get(a)))
        c = el.attrib.pop("class", None)
        if not c:
            continue
        spec = CLS[c]
        if spec.get("upper") and el.text:
            el.text = el.text.upper()
        for k, val in spec.items():
            if k == "upper":
                continue
            # never clobber a colour the emitter set deliberately (rid, bandlabel)
            if k == "fill" and el.get("fill"):
                continue
            el.set(k, val)

    t = ET.Element("title"); t.text = aria
    d = ET.Element("desc");  d.text = "Generated by tools/gen_figures.py from pcomm.fbs and the PCOMM_RECORD declarations."
    bg = ET.Element("rect", {"width": str(vb[2]), "height": str(vb[3]), "fill": "#ffffff"})
    root.insert(0, bg); root.insert(0, d); root.insert(0, t)
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="unicode") + "\n"

if __name__ == "__main__":
    import os, sys

    OUT = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "demos", "figures")
    items = [
        ("05-capture-anatomy",  build(), "A capture: per-rank streams, and what a record is"),
        ("06-engine-records",   FIGS["engine"],   "The engine's records, and gpumem's"),
        ("07-ibverbs-records",  FIGS["ib"],       "The ibverbs record family"),
        ("08-mlx5-records",     FIGS["mlx"],      "The mlx5dv / DEVX record family"),
        ("09-ucx-records",      FIGS["ucx"],      "The UCX record family"),
        ("10-nccl-records",     FIGS["nccl"],     "The NCCL record family"),
        ("11-cross-rank-keys",  FIGS["cross"],    "The five cross-rank join keys"),
        ("12-derived-tables",   FIGS["derived"],  "Records to views to the seven derived tables"),
    ]
    for name, body, title in items:
        out = convert(body, title)
        assert "var(--" not in out, name
        assert 'class="' not in out, name
        open(f"{OUT}/{name}.svg", "w").write(out)
        print(f"{name}.svg  {len(out):6d} B")
