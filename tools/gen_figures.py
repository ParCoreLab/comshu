#!/usr/bin/env python3
"""Emit the record-model figures in demos/figures (05 through 12) as standalone SVG.

These are the ER half of the talk figures: 04-record-model.svg says what the four
archetypes ARE, and these say which records exist, what keys them, and which join
carries one to the next. There are 64 record types across six provider ranges, which
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

# What the figures actually drew, filled in by box(). The index table at the
# foot of this file and the boxes in the diagrams are two hand-maintained lists
# of the same 64 records, and nothing but this registry connects them —
# tools/check_figures.py compares both against the PCOMM_RECORD declarations.
DRAWN = {}
CX, CY = 290, 70        # column / row step

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def box(x, y, name, tid, kind, fields, fam, w=W):
    """One record: name, type_id, kind tag, key fields."""
    # The field line runs until the kind tag; past ~30 chars the two collide.
    assert len(fields) <= 30, f"{name}: field line is {len(fields)} chars, max 30"
    assert len(name) <= 22, f"{name}: name is {len(name)} chars, max 22"
    # kind is "" for the cross-rank and derived figures, which redraw records
    # already drawn in their family figure or name views rather than records.
    if kind:
        assert name not in DRAWN, f"{name}: drawn in two family figures"
        DRAWN[name] = (tid, kind)
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
b.append(band(8, 392, 844, 100, "int", "intern dictionary — every Handle in every record, in every family, resolves here"))
b.append(box(300, 40, "gpu_access", 700, "EVT", "pc · object · address · launch", "gpu"))
b.append(box(10, 40, "mem_object", 520, "ENT", "id · range · device", "int"))
b.append(box(590, 40, "gpu_kernel", 530, "EVT", "correlation · stream · name", "int"))
b.append(box(300, 145, "source_line", 740, "ART", "pc · file · line", "gpu"))
b.append(box(10, 145, "peer_mapping", 526, "ENT", "allocation · range · offset", "int"))
b.append(box(590, 145, "attribution", 760, "EDG", "subject · object · pred", "gpu"))
b.append(box(10, 230, "node_identity", 524, "ART", "node_hash", "int"))
b.append(box(300, 230, "entity_close", 522, "EVT", "subject · opener", "int"))
b.append(box(590, 230, "clock_anchor", 590, "ART", "domain · local_ticks · wall_ns", "int"))
b.append(box(10, 316, "gpu_device", 528, "ART", "ordinal · pci_bus · uuid", "int"))
b.append(box(300, 316, "invocation", 532, "ART", "argv · env · build · images", "int"))
b.append(box(150, 422, "intern_key", 510, "ART", "key → handle · domain", "int"))
b.append(box(450, 422, "intern_string", 512, "ART", "offset · length → blob", "int"))
b.append(path("M300 63 H245", "object → id", 272, 32))
b.append(path("M530 63 H585", "launch → correlation", 558, 32))
b.append(path("M415 86 V140", "pc", 423, 116, anchor="start"))
b.append(path("M125 145 V90", "peer addr → owner base", 133, 122, anchor="start"))
b.append(path("M415 276 V386", None))
b.append(path("M705 276 V386", None))
# node_identity -> gpu_device, and the arrow is the whole point of the record:
# gpu_device names a card by PCI address, which is unique on ONE machine. The
# node_hash above it is what makes that name unique across the capture.
b.append(path("M125 276 V312", "ordinals and PCI addresses are node-local", 133, 298, anchor="start"))
b.append(path("M125 362 V386", None))
b.append(note(10, 510, "entity_close is one shared type for every provider: subject is the interned handle the OPEN record carried,", cls="fnote"))
b.append(note(10, 524, "and opener is that record's type_id. A close naming a subject nothing opened is not a bug in the close hook —", cls="fnote"))
b.append(note(10, 538, "it is evidence of a creation door that is not hooked, and it is counted rather than suppressed.", cls="fnote"))
FIGS["engine"] = svg(860, 552, "".join(b),
    "The engine's records in the 500 range and the gpumem provider's in the 700 range, with the intern dictionary every handle resolves through")

# ---------------------------------------------------------------- fig: ibverbs
b = []
b.append(band(292, 178, 250, 298, "ib", "pd handle · nothing creates it", label_at="bottom"))
b.append(box(10, 60, "ib_device", 120, "ENT", "context · node_guid · fw_ver", "ib"))
b.append(box(10, 140, "ib_port", 122, "ENT", "context · lid · port_num", "ib"))
b.append(box(300, 40, "ib_completion_queue", 104, "ENT", "cq · context · cqe", "ib"))
b.append(box(300, 110, "ib_qp_signalling", 126, "ART", "qp · qp_num · sq_sig_all", "ib"))
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
b.append(box(300, 145, "ucx_worker_address", 622, "ART", "worker · prefix_hash", "ucx"))
b.append(box(300, 215, "ucx_tag_recv", 608, "EVT", "worker · tag · tag_mask", "ucx"))
b.append(box(300, 285, "ucx_tag_probe", 618, "EVT", "worker · message · length", "ucx"))
b.append(box(300, 355, "ucx_worker_progress", 616, "EVT", "worker · progressed", "ucx"))
b.append(box(620, 145, "ucx_ep_connection", 624, "ART", "endpoint · remote_prefix_hash", "ucx"))
b.append(box(620, 215, "ucx_tag_send", 606, "EVT", "endpoint · tag · buffer", "ucx"))
b.append(box(620, 285, "ucx_rma", 610, "EVT", "endpoint · op · remote_addr", "ucx"))
b.append(box(620, 355, "ucx_atomic", 620, "EVT", "endpoint · op · remote_addr", "ucx"))
b.append(box(620, 425, "ucx_am_send", 612, "EVT", "endpoint · am_id · header_len", "ucx"))
b.append(box(300, 495, "ucx_rkey_pack", 628, "ART", "memh · buffer_hash · len", "ucx"))
b.append(box(620, 495, "ucx_rkey", 626, "ENT", "rkey · endpoint", "ucx"))
b.append(path("M300 63 H245", "context", 272, 32))
b.append(path("M620 63 H555", "worker", 587, 32))
b.append(path("M125 145 V90", "context", 133, 122, anchor="start"))
b.append(stack_up([(300, 168), (300, 238), (300, 308), (300, 378)], 268, 415, 86, "worker"))
b.append(stack_up([(620, 168), (620, 238), (620, 308), (620, 378), (620, 448), (620, 518)],
                  588, 735, 86, "endpoint"))
# The gap between the two rkey boxes is the finding, so it is drawn as a gap:
# a dashed line with no arrowhead. The exporting side and the consuming side
# hold the same bytes and cannot be joined, because ucp_ep_rkey_unpack takes no
# length and a packed rkey may be 9 bytes. See UcxRkey in ucx_records.hpp.
b.append(path("M530 518 H620", "no join", 575, 510, dashed=True, head=False))
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
# ucx_deep, 800-899: the layer BELOW everything above. Drawn in the same figure
# on purpose — the whole point of the family is that it sits under the UCP
# records, and a separate page would hide the one relationship that matters.
b.append(band(8, 570, 864, 152, "uct", "ucx_deep / UCT 800\u2013899 \u2014 what UCX did about it"))
b.append(box(20, 594, "uct_iface", 800, "ENT", "iface \u00b7 tl_name \u00b7 dev_name", "uct"))
b.append(box(300, 594, "uct_ep", 802, "ENT", "ep \u00b7 iface", "uct"))
b.append(box(580, 594, "uct_send", 804, "EVT", "ep \u00b7 op \u00b7 payload \u00b7 tag", "uct"))
b.append(box(580, 664, "uct_binding", 806, "ART", "runtime \u00b7 accepted", "uct"))
b.append(path("M300 617 H255", "iface", 277, 586))
b.append(path("M580 617 H535", "ep", 557, 586))
b.append(note(20, 680, "The 600s and the 800s are joined on the TAG, by the reader's tag_flow, not by", cls="fnote"))
b.append(note(20, 694, "a field \u2014 the join is per (rank, tag), never per message. Untagged doors carry", cls="fnote"))
b.append(note(20, 708, "the tag inside an AM payload and stay unjoinable; uct_ep is a LANE, not a QP.", cls="fnote"))
FIGS["ucx"] = svg(880, 734, "".join(b),
    "The UCX provider's fifteen record types on the context, worker, endpoint ownership spine, and the four ucx_deep records beneath them")

# ---------------------------------------------------------------- fig: nccl
b = []
b.append(box(10, 130, "nccl_comm", 900, "ENT", "comm · comm_hash · rank", "ncc"))
b.append(box(360, 40, "nccl_collective", 902, "EVT", "comm · count · root · op", "ncc"))
b.append(box(360, 110, "nccl_p2p", 904, "EVT", "comm · peer · dir · count", "ncc"))
b.append(box(360, 180, "nccl_window", 908, "ENT", "window · comm · buffer · size", "ncc"))
b.append(box(360, 250, "nccl_dev_comm", 910, "ART", "comm", "ncc"))
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
# The profiler-plugin layer. A SECOND column rather than more boxes hanging off
# the communicator, because these do not join through `comm` at all: the plugin
# is never told a ncclComm_t, only NCCL's own commHash — which is why nccl_comm
# carries that hash as a second, differently-named column and why the rail below
# leaves from it and not from the comm pointer.
b.append(box(620, 40, "nccl_prof_comm", 912, "ART", "nccl_comm_hash · nranks", "ncc"))
b.append(box(620, 130, "nccl_coll_exec", 914, "EVT", "algo · proto · n_channels", "ncc"))
b.append(box(620, 220, "nccl_proxy_op", 916, "EVT", "peer · channel · n_steps", "ncc"))
b.append(path("M236 143 H616", "nccl_comm_hash", 420, 138, anchor="middle"))
b.append(path("M733 63 V126", None))
b.append(path("M733 153 V216", "parent_event_id", 737, 190, anchor="start"))
b.append(note(620, 262, "What NCCL DECIDED, not what was asked. The", cls="fnote"))
b.append(note(620, 276, "temporal here is the collective's own lifetime,", cls="fnote"))
b.append(note(620, 290, "NOT the enqueue on the left.", cls="fnote"))
b.append(note(620, 312, "parent_event_id is NCCL's OWN linkage handed", cls="fnote"))
b.append(note(620, 326, "back to us — not reconstructed from timing.", cls="fnote"))
b.append(note(620, 348, "No proxy op exists for device-initiated or", cls="fnote"))
b.append(note(620, 362, "NVLink traffic: it never reaches the CPU proxy.", cls="fnote"))
FIGS["nccl"] = svg(1060, 400, "".join(b),
    "The NCCL provider's nine record types: six host-API records hanging off the communicator, and three profiler-plugin records carrying what NCCL chose")

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
        ("invocation", 532, "int"), ("clock_anchor", 590, "int")]
sy = []
for i, (n, t, f) in enumerate(srcs):
    y = 40 + i * 54
    b.append(box(20, y, n, t, "", "", f, w=196))
    sy.append((216, y + 23))
views = ["_sends", "_rtr  ·  _owner", "_peer_of_qp", "_wr_pairable  ·  _wr_flight",
         "_nvlink_sends", "_node  ·  _device_owner", "_ucx_sends  ·  _ucx_peer_of_ep",
         "_intern_name  ·  _uct_scoped", "_ucp_tag_flow  ·  _uct_tag_flow",
         "_ucx_msg_recv  ·  _ucx_probe_of_recv", "_recv_attr", "_tl_raw", "_clock"]
vy = []
for i, n in enumerate(views):
    y = 42 + i * 45
    b.append(f'<rect x="330" y="{y}" width="222" height="38" rx="3" fill="var(--surface)" '
             f'stroke="var(--rule-strong)" stroke-width="1.2" stroke-dasharray="4 3"/>'
             f'<text x="{342}" y="{y+24}" class="rname">{esc(n)}</text>')
    vy.append((y + 19))
tables = [("comm_edges", "src → dst, per transport"), ("edge_sizes", "the same edges, split by size class"),
          ("endpoint", "one row per local queue pair"),
          ("object_access", "every touch, as an offset in an object"), ("wr_flow", "posted → retired, with latency"),
          ("timeline_span", "every event on one wall clock"), ("rank_identity", "rank → node, device"),
          ("transport_mix", "which transport actually carried it"),
          ("tag_flow", "what a UCP send became on the wire"),
          ("tag_recv_resolved", "the tag a msg_recv did not carry"),
          ("nccl_execution", "what NCCL CHOSE — algo, proto"),
          ("nccl_peer_traffic", "where a collective became peer traffic"),
          ("run_provenance", "what the TRACER was asked to do"),
          ("vocabulary", "every enum value, by name"),
          ("link_health", "what the graph could NOT account for")]
# Bound to its own name because the header's count is rendered from it. A
# hardcoded 7 sat there while this list grew to fourteen — the same drift the
# record counts had, and the reason check_figures.py exists.
DERIVED_TABLES = tables
ty = []
for i, (n, d) in enumerate(tables):
    y = 24 + i * 50
    b.append(f'<rect x="660" y="{y}" width="252" height="46" rx="3" fill="var(--surface)" '
             f'stroke="var(--fam-int)" stroke-width="1.4"/>'
             f'<rect x="660" y="{y}" width="3.5" height="46" rx="1.5" fill="var(--fam-int)"/>'
             f'<text x="673" y="{y+20}" class="rname">{esc(n)}</text>'
             f'<text x="673" y="{y+36}" class="rfield">{esc(d)}</text>')
    ty.append((656, y + 23))
b.append(bus(sy, 272, [(326, v) for v in vy]))
b.append(bus([(552, v) for v in vy], 606, ty))
b.append(note(20, 766, "records, as written", cls="collabel"))
b.append(note(330, 766, "views — created in order", cls="collabel"))
b.append(note(660, 766, "what the panels and the suite read", cls="collabel"))
FIGS["derived"] = svg(930, 780, "".join(b),
    "How the reader turns raw per-rank records into the fifteen derived tables that the UI, the agent and the test suite query")

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
    "--fam-uct": "#2f6ea8", "--fam-uct-wash": "#eef4fa",
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
    d = ET.Element("desc");  d.text = ("Hand-authored layout, emitted by tools/gen_figures.py. The record set was "
              "cross-checked against the PCOMM_RECORD declarations by hand; nothing enforces it.")
    bg = ET.Element("rect", {"width": str(vb[2]), "height": str(vb[3]), "fill": "#ffffff"})
    root.insert(0, bg); root.insert(0, d); root.insert(0, t)
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="unicode") + "\n"


# ------------------------------------------------- the page: demos/entity-map.html
#
# The figures say which records exist and how they join. The page is what carries
# the rest: the archetype legend, the full index of all 53 with their type_ids, and
# the list of places the model is still open. It <img>s the committed SVGs rather
# than inlining a copy of them, so there is exactly one of each figure in the tree.
#
# No webfonts, no CDN, no script: MN5's login node is airgapped and an external
# stylesheet there is a silent fallback, which is the same failure shape the schema
# hash exists to make loud.

SANS_STACK = ('ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, '
              '"Helvetica Neue", Arial, sans-serif')
MONO_STACK = ('ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, '
              '"Liberation Mono", monospace')

# name, type_id, kind, one line. Cross-checked against the PCOMM_RECORD declarations
# in libpcomm/, providers/ and gpu-backends/ — by tools/check_figures.py on every
# commit, not "when written". The three counts in this file said 53 while the tree
# held 60, because a comment is not a check and drifted the moment one was added.
IB = [
 ("ib_mem_region", 100, "Entity", "A registered region, [reg, dereg). lkey/rkey are what a send or a receive resolves its buffer through."),
 ("ib_queue_pair", 102, "Entity", "A queue-pair endpoint, [create, destroy)."),
 ("ib_completion_queue", 104, "Entity", "A completion queue. Hangs off a context, not a pd \u2014 the one verbs object that can exist before any protection domain."),
 ("ib_modify_qp", 106, "Event", "A state transition. The \u2192RTR row carries the remote endpoint, which is what makes the connection topology recoverable."),
 ("ib_shared_recv_queue", 108, "Entity", "A shared receive queue. Depths are the device's answers, not the caller's request."),
 ("ib_post_send", 110, "Event", "One posted work request \u2014 the network firehose."),
 ("ib_poll_cq", 112, "Event", "One completion drained from a CQ."),
 ("ib_post_recv", 114, "Event", "One posted receive. addr/lkey resolve the receive buffer to a region \u2014 the question a capture could not answer before this record."),
 ("ib_req_notify_cq", 116, "Event", "A CQ armed for notification: the only evidence an application sleeps on interrupts rather than spinning."),
 ("ib_address_handle", 118, "Entity", "A UD address handle. Carries the destination a UD send is aimed at, which lives nowhere else in the API."),
 ("ib_device", 120, "Entity", "The HCA behind one context. node_guid is the globally unique name a LID only approximates."),
 ("ib_port", 122, "Entity", "One physical port and its LID, read at device open, so a rank that never moves traffic still has one."),
 ("ib_recv_cqe", 124, "Event", "The raw CQE's own account of a receive \u2014 written only where it agreed with the ibv_wc beside it."),
 ("ib_qp_signalling", 126, "Artifact", "sq_sig_all, which certifies that every work request completes. Additive, because a layout is immutable once a capture holds it."),
 ("ib_post_send_sg", 128, "Event", "One scatter-gather entry \u2014 the LOCAL buffer, which ib_post_send does not keep. Full mode only."),
]
MLX = [
 ("mlx5_umem", 200, "Entity", "A buffer handed to the NIC. base is 0 under dmabuf: the buffer is named by fd and the address is an offset."),
 ("mlx5_uar", 202, "Entity", "A User Access Region \u2014 the doorbell page a GPU writes to say a WQE is ready."),
 ("mlx5_devx_cmd", 204, "Event", "One DEVX command, decoded or not. Recorded even when the opcode is unknown, so a version boundary is visible."),
 ("mlx5_devx_qp", 206, "Entity", "A DEVX queue pair \u2014 the GPU-initiated counterpart of ib_queue_pair."),
 ("mlx5_devx_qp_connect", 208, "Event", "The \u2192RTR transition on the DEVX plane, carrying src_lid for the same reason verbs does."),
 ("mlx5_queue_pair", 210, "Entity", "An ordinary verbs QP reached through mlx5dv_create_qp. perftest creates its queue pairs this way and never calls ibv_create_qp."),
 ("mlx5_doorbell", 212, "Event", "The GPU rang a doorbell. Emitted even when the WQE cannot be read, so an undecodable doorbell is a visible row."),
 ("mlx5_device_send", 214, "Event", "A GPU-initiated send decoded from the WQE. Same shape as ib_post_send so a reader can union the two planes."),
]
INT = [
 ("intern_key", 510, "Artifact", "handle \u2192 uint64 for the numeric domains. The dictionary behind every Handle."),
 ("intern_string", 512, "Artifact", "handle \u2192 (offset, length) into intern-blob.bin. Strings are the one thing that cannot sit in a fixed-stride record."),
 ("mem_object", 520, "Entity", "A memory object, [alloc, free). Internal rather than gpumem's range because any provider holding an address may emit one."),
 ("entity_close", 522, "Event", "An entity's lifetime ended. One shared type for every provider; opener is the type_id of the record that should have opened it."),
 ("node_identity", 524, "Artifact", "Which machine this rank ran on. A device ordinal is node-local, so run_device 0 on two nodes is two different GPUs."),
 ("peer_mapping", 526, "Entity", "One process's mapping of a shared allocation, keyed by the CUDA fabric handle \u2014 FABRIC only, because a POSIX fd names the driver node."),
 ("gpu_device", 528, "Artifact", "One visible GPU, named by PCI address. What a device ordinal actually points at, so two ranks that both say 'device 0' can be told apart."),
 ("invocation", 532, "Artifact", "What the TRACER was asked to do — argv, the captured environment, the providers that loaded, the build, and which .so files were actually mapped."),
 ("gpu_kernel", 530, "Event", "One kernel's EXECUTION window. Never submission: the two are ~800x the kernel's own duration apart."),
 ("clock_anchor", 590, "Artifact", "A clock-domain calibration anchor. Two per rank per domain, which is exactly what a line needs."),
]
UCX = [
 ("ucx_context", 600, "Entity", "A public UCP context."),
 ("ucx_worker", 602, "Entity", "A worker owned by a context."),
 ("ucx_endpoint", 604, "Entity", "An endpoint owned by a worker."),
 ("ucx_tag_send", 606, "Event", "A tagged-send intent. count is the raw UCP datatype count."),
 ("ucx_tag_recv", 608, "Event", "A tagged receive POSTED \u2014 capacity offered, not bytes delivered. Keyed by worker: the match is by tag across every peer."),
 ("ucx_rma", 610, "Event", "A one-sided put or get."),
 ("ucx_am_send", 612, "Event", "An active-message send. Header and payload keep separate lengths \u2014 one is control traffic, the other is data."),
 ("ucx_mem_map", 614, "Entity", "A UCP memory registration. resolved says whether the extent is the effective one or the requested one."),
 ("ucx_worker_progress", 616, "Event", "A progress call that progressed something. Full mode only, and telemetry rather than a golden."),
 ("ucx_tag_probe", 618, "Event", "A probe that FOUND a message. length is the one true byte count in the UCX family."),
 ("ucx_atomic", 620, "Event", "A remote atomic, with op NORMALISED across the three UCX opcode spaces that number the same operations differently."),
 ("ucx_worker_address", 622, "Artifact", "This worker's own address, as two hashes: the prefix a cross-rank join uses, and a full one that makes a prefix collision visible."),
 ("ucx_ep_connection", 624, "Artifact", "Which remote worker an endpoint was dialled at."),
 ("ucx_rkey", 626, "Entity", "A remote key, lifetime [ucp_ep_rkey_unpack, ucp_rkey_destroy). What ucx_rma.rkey and ucx_atomic.rkey join to; before it, they named a handle nothing created."),
 ("ucx_rkey_pack", 628, "Artifact", "A registration packed for export. Carries the whole buffer's hash, which the consuming side cannot compute — ucp_ep_rkey_unpack takes no length."),
]
UCT = [
 ("uct_iface", 800, "Entity", "One (transport, device) interface. tl_name/dev_name are the only place a capture says whether a UCX message left the node."),
 ("uct_ep", 802, "Entity", "One UCT endpoint \u2014 a LANE of a ucp_ep, not a queue pair. A ucp_ep fans out over several, possibly on different transports."),
 ("uct_send", 804, "Event", "One UCT data operation: the wire, as close as a host interposition reaches. payload means sent, requested or advertised depending on the op family, and must not be summed across them."),
 ("uct_binding", 806, "Artifact", "Which UCX this bound to and whether the layout guard allowed patching. Read it first when uct_send is empty."),
]
GPU = [
 ("gpu_access", 700, "Event", "One GPU memory access \u2014 the firehose, one row per instrumented SASS load or store."),
 ("source_line", 740, "Artifact", "One pc \u2192 source location row. Needs -lineinfo; empty without it."),
 ("attribution", 760, "Edge", "The generic relation row, for links too sparse to inline: subject, object, and a predicate."),
]
NCC = [
 ("nccl_comm", 900, "Entity", "A communicator. comm_hash is equal on every rank; the comm pointer is not, so the hash is the only cross-rank join."),
 ("nccl_collective", 902, "Event", "One enqueued collective. count is ELEMENTS, and the product with the datatype width is LOGICAL size \u2014 not what crosses the wire."),
 ("nccl_p2p", 904, "Event", "The only NCCL call that names a peer, and so the only one an exact edge follows from."),
 ("nccl_group", 906, "Event", "Group boundaries: N grouped sends are ONE launch, and nothing in a send row says so."),
 ("nccl_window", 908, "Entity", "A registered symmetric window \u2014 what makes GPU-initiated NCCL attributable where internal channel buffers are not."),
 ("nccl_dev_comm", 910, "Artifact", "The existence fact 'this workload initiates communication from the GPU'. Nothing else in a capture states it."),
 ("nccl_prof_comm", 912, "Artifact", "The communicator as the profiler plugin was told about it. Carries NCCL's own commHash — the only name the plugin is ever given, and so the only join back to nccl_comm."),
 ("nccl_coll_exec", 914, "Event", "The collective as NCCL EXECUTED it: the algorithm and protocol it chose. Its temporal is the operation's lifetime inside NCCL, NOT nccl_collective's enqueue."),
 ("nccl_proxy_op", 916, "Event", "Where a collective becomes per-peer traffic, linked to it by NCCL's own parent id. n_steps × chunk_size is what NCCL PLANNED, not bytes a completion confirmed."),
]
FAMILIES = [
 ("int", "the engine", "500\u2013599", INT),
 ("gpu", "gpumem", "700\u2013799", GPU),
 ("ib", "ibverbs", "100\u2013199", IB),
 ("mlx", "mlx5dv / DEVX", "200\u2013299", MLX),
 ("ucx", "ucx", "600\u2013699", UCX),
 ("uct", "ucx_deep", "800\u2013899", UCT),
 ("ncc", "nccl", "900\u2013999", NCC),
]
KINDTAG = {"Entity": "ENT", "Event": "EVT", "Artifact": "ART", "Edge": "EDG"}

PAGE_FIGURES = [
 ("05-capture-anatomy", "What holds them",
  "No record is self-describing on its own. A record is a fixed-stride element of a packed array; its type "
  "comes from the stream header above it, and its high-cardinality fields are dense handles that mean nothing "
  "without the capture's own dictionary.",
  "A capture is one directory per rank. Lookups into the dictionary are keyed by <b>(domain, handle)</b>, not "
  "handle alone: handles restart at 1 in every domain."),
 ("06-engine-records", "The engine's records, and gpumem's",
  "Records every provider needs and none owns. <code>mem_object</code> sits here rather than in gpumem's range "
  "because any provider holding an address may emit one; <code>gpu_kernel</code> sits here because the fact has "
  "two possible instruments and a capture must know it under either.",
  "The three arrows out of <code>gpu_access</code> are the whole of its attribution: an object, a kernel launch, "
  "and a source line. <code>launch</code> is a join key and not a time \u2014 the producer records the key and the "
  "reader does the join."),
 ("07-ibverbs-records", "ibverbs",
  "LD_PRELOAD over libibverbs: the host control and data plane, as the application called it. Fifteen record "
  "types, the largest family in the model.",
  "The dashed enclosure is a <b>gap, not a grouping</b>: four of these entities carry a <code>pd</code> handle "
  "and no record ever creates a protection domain, so a query can group by it but never name it."),
 ("08-mlx5-records", "mlx5dv / DEVX",
  "The control plane IBGDA uses instead of verbs. A GPU-initiated run creates its queue pairs with "
  "<code>mlx5dv_devx_obj_create</code>, so nothing in the ibverbs range sees any of it.",
  "<code>mlx5_device_send</code> is deliberately the same shape as <code>ib_post_send</code> so the reader can "
  "union them and a consumer of the comm matrix never learns which plane a run used."),
 ("09-ucx-records", "ucx, and ucx_deep beneath it",
  "UCP transport hooks. The spine is ownership \u2014 a context owns workers, a worker owns endpoints \u2014 and every "
  "operation attaches to whichever of the two the API itself names. The band below is the UCT layer, reached "
  "through uct_iface_ops_t: on the HPC-X stack it is the only data plane a capture has, because UCX rings its "
  "own doorbell and neither ib_post_send nor mlx5_device_send ever fires.",
  "Which side an operation is keyed to is the API's own shape, not a convention: a tagged send names its "
  "endpoint, while a tagged receive cannot, because it matches by tag across every peer at once. The two bands "
  "are joined by the reader, not by a field: <code>tag_flow</code> pairs a <code>ucx_tag_send</code> with the "
  "<code>uct_send</code> rows it became, on the tag, per (rank, tag) rather than per message — a tag is reused, "
  "and pairing individual operations would need an ordering assumption between two layers that buffer "
  "independently. Traffic through untagged doors carries its tag inside an AM payload and stays unjoinable; "
  "<code>transport_mix</code> accounts for that by transport instead."),
 ("10-nccl-records", "nccl",
  "The one provider with two doors, and they answer different questions. Six host-API record types hang off "
  "the communicator and say what the application <b>asked for</b>; three profiler-plugin records say what NCCL "
  "<b>decided</b> — the algorithm, the protocol, and the decomposition into per-peer proxy operations.",
  "The left column is entirely <b>enqueue</b> facts: NCCL is stream-ordered, so a collective's temporal there is "
  "the host call, not the execution. The right column's temporals are the operation's own lifetime inside NCCL. "
  "Reporting either as the other invents a number. The two columns join on NCCL's own commHash, not on the "
  "comm pointer, because the plugin is never told one."),
 ("11-cross-rank-keys", "The five cross-rank keys",
  "Everything above is rank-local. A capture is N independent directories written by N processes that never "
  "coordinate, so joining them needs an identity both sides mint independently and agree on. There are exactly five.",
  "Where a key is absent the reader does not guess. A pure-NVLink capture has no LID to derive a node from, and "
  "that shows up as a counted row in <code>link_health</code> rather than as a confident matrix over a hole."),
 ("12-derived-tables", "What the reader builds",
  "The records are what a capture holds; these eleven tables are what anything actually queries \u2014 the UI panels, "
  "the suite's assertions, the agent's SQL. They are outputs of the reader rather than parts of the format, "
  "which is why they carry no schema.",
  "Dashed boxes are intermediate views, created in order, each free to reference the ones above it. "
  "<code>link_health</code> is the one table whose expected state is empty."),
]

PAGE_CSS = """
:root {
  --bg:#ffffff; --fg:#1c2b36; --muted:#5f7183; --rule:#e2e8ed; --panel:#f7f9fb;
  --figure-ground:#ffffff;
  --int:#5b7c99; --gpu:#4f7d3f; --ib:#a06a24; --mlx:#6a5aa0; --ucx:#1f7a70; --uct:#2f6ea8; --ncc:#a04a58;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg:#12181e; --fg:#dde5ec; --muted:#93a3b1; --rule:#26313b; --panel:#182029;
    --figure-ground:#f3f6f8;
    --int:#7fadd6; --gpu:#8cc172; --ib:#d79f5c; --mlx:#a795dd; --ucx:#4fb3a6; --uct:#7cb0e0; --ncc:#dd8894;
  }
}
* { box-sizing: border-box; }
html { color-scheme: light dark; }
body {
  margin:0; background:var(--bg); color:var(--fg);
  font-family:SANS; font-size:16px; line-height:1.62;
  -webkit-text-size-adjust:100%;
}
.wrap { max-width:1120px; margin:0 auto; padding:0 24px 96px; }
.prose { max-width:70ch; }
h1,h2,h3 { line-height:1.15; margin:0; }
h1 { font-size:2.6rem; font-weight:700; letter-spacing:-.022em; }
h2 { font-size:1.4rem; font-weight:650; letter-spacing:-.012em; }
h3 { font-size:1rem; font-weight:650; }
p { margin:0; }
code { font-family:MONO; font-size:.87em; background:var(--panel);
       border:1px solid var(--rule); border-radius:3px; padding:.05em .32em; }
a { color:inherit; }
header { padding:72px 0 36px; border-bottom:1px solid var(--rule); }
.eyebrow { font-family:MONO; font-size:.7rem; letter-spacing:.16em; text-transform:uppercase;
           color:var(--muted); margin-bottom:14px; }
.lede { margin-top:20px; font-size:1.1rem; }
.counts { display:flex; flex-wrap:wrap; margin-top:34px; border:1px solid var(--rule);
          border-radius:5px; overflow:hidden; background:var(--panel); }
.counts div { flex:1 1 110px; padding:13px 16px; border-right:1px solid var(--rule); }
.counts div:last-child { border-right:0; }
.counts .n { font-family:MONO; font-size:1.4rem; font-variant-numeric:tabular-nums; line-height:1.1; }
.counts .l { font-family:MONO; font-size:.64rem; letter-spacing:.11em; text-transform:uppercase;
             color:var(--muted); margin-top:4px; }
section { padding-top:60px; }
.sub { color:var(--muted); margin-top:9px; max-width:72ch; }
figure { margin:26px 0 0; padding:18px; background:var(--figure-ground);
         border:1px solid var(--rule); border-radius:6px; overflow-x:auto; }
figure img { display:block; width:100%; min-width:660px; height:auto; }
figcaption { margin-top:15px; padding-top:13px; border-top:1px solid var(--rule);
             color:#5f7183; font-size:.86rem; max-width:84ch; }
.legend { display:grid; grid-template-columns:repeat(auto-fit,minmax(205px,1fr)); gap:1px;
          background:var(--rule); border:1px solid var(--rule); border-radius:5px;
          overflow:hidden; margin-top:26px; }
.legend > div { background:var(--panel); padding:14px 16px; }
.legend .tag { font-family:MONO; font-size:.64rem; letter-spacing:.11em; color:var(--muted); }
.legend .nm { font-family:MONO; font-weight:600; font-size:.9rem; margin-top:3px; }
.legend .say { font-size:.82rem; color:var(--muted); margin-top:5px; line-height:1.45; }
.tblwrap { overflow-x:auto; }
table { width:100%; border-collapse:collapse; margin-top:22px; font-size:.85rem; }
caption { text-align:left; color:var(--muted); font-size:.84rem; padding-bottom:12px; max-width:80ch; }
th { text-align:left; font-family:MONO; font-size:.64rem; letter-spacing:.11em; text-transform:uppercase;
     color:var(--muted); font-weight:500; padding:0 12px 8px 0; border-bottom:1px solid var(--muted); }
td { padding:9px 12px 9px 0; border-bottom:1px solid var(--rule); vertical-align:top; }
tr.fam td { padding-top:26px; border-bottom:1px solid var(--muted); }
tr.fam .fname { font-weight:650; font-size:1rem; }
td.id { font-family:MONO; font-variant-numeric:tabular-nums; width:1%; white-space:nowrap; font-weight:500; }
td.nm { font-family:MONO; font-weight:600; width:1%; white-space:nowrap; }
td.kd { width:1%; white-space:nowrap; }
td.kd span { font-family:MONO; font-size:.62rem; letter-spacing:.09em; border:1px solid var(--muted);
             color:var(--muted); border-radius:3px; padding:.15em .4em; }
td.ds { color:var(--muted); line-height:1.5; }
.holes { margin-top:24px; display:grid; gap:1px; background:var(--rule);
         border:1px solid var(--rule); border-radius:5px; overflow:hidden; }
.holes > div { background:var(--panel); padding:16px 18px; }
.holes p { color:var(--muted); font-size:.87rem; margin-top:5px; }
footer { margin-top:76px; padding-top:20px; border-top:1px solid var(--rule);
         color:var(--muted); font-size:.8rem; max-width:80ch; }
@media (max-width:720px) { h1 { font-size:2rem; } .wrap { padding:0 16px 64px; } }
"""


def page_html():
    kinds = {}
    for _, _, _, recs in FAMILIES:
        for _, _, k, _ in recs:
            kinds[k] = kinds.get(k, 0) + 1
    total = sum(kinds.values())

    figs_html = []
    for slug, title, blurb, caption in PAGE_FIGURES:
        figs_html.append(
            f'<section id="{slug}">\n  <h2>{title}</h2>\n  <p class="sub">{blurb}</p>\n'
            f'  <figure>\n    <img src="figures/{slug}.svg" alt="{title}">\n'
            f'    <figcaption>{caption}</figcaption>\n  </figure>\n</section>')

    rows = []
    for key, title, rng, recs in FAMILIES:
        rows.append(f'<tr class="fam"><td colspan="4"><span class="fname" style="color:var(--{key})">'
                    f'{title}</span> <span style="color:var(--muted);font-size:.8rem">&nbsp;{rng}</span></td></tr>')
        for name, tid, kind, desc in recs:
            rows.append(f'<tr><td class="id" style="color:var(--{key})">{tid}</td><td class="nm">{name}</td>'
                        f'<td class="kd"><span>{KINDTAG[kind]}</span></td><td class="ds">{desc}</td></tr>')

    css = PAGE_CSS.replace("SANS", SANS_STACK).replace("MONO", MONO_STACK)
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>pcomm entity map</title>
<style>{css}</style>
</head>
<body>
<div class="wrap">

<header>
  <div class="eyebrow">pcomm &middot; the record model, concretely</div>
  <h1>The entity map</h1>
  <div class="prose">
    <p class="lede">Every entity a pcomm capture can hold, what it is keyed by, and which join carries it to
    the next one. Four archetypes, six provider ranges, {total} record types &mdash; and five keys that turn N
    per-rank directories into one picture.</p>
  </div>
  <div class="counts">
    <div><div class="n">{total}</div><div class="l">record types</div></div>
    <div><div class="n" style="color:var(--int)">{kinds['Entity']}</div><div class="l">entity</div></div>
    <div><div class="n" style="color:var(--ib)">{kinds['Event']}</div><div class="l">event</div></div>
    <div><div class="n" style="color:var(--ucx)">{kinds['Artifact']}</div><div class="l">artifact</div></div>
    <div><div class="n" style="color:var(--ncc)">{kinds['Edge']}</div><div class="l">edge</div></div>
    <div><div class="n">{len(DERIVED_TABLES)}</div><div class="l">derived tables</div></div>
  </div>
</header>

<section id="read">
  <h2>How to read a box</h2>
  <p class="sub">Every box is one record type. The border colour is its <em>provider range</em>, which is a real
  constraint and not a decoration: each provider owns a disjoint block of type_ids, which is what lets two passes
  over the same workload merge into one rank's picture without either knowing about the other.</p>
  <div class="legend">
    <div><div class="tag">ENT</div><div class="nm">Entity</div>
      <div class="say">A thing with a lifetime. It opens, and it should close.</div></div>
    <div><div class="tag">EVT</div><div class="nm">Event</div>
      <div class="say">Something that happened, at or over a time.</div></div>
    <div><div class="tag">ART</div><div class="nm">Artifact</div>
      <div class="say">A one-off fact, computed once and timeless.</div></div>
    <div><div class="tag">EDG</div><div class="nm">Edge</div>
      <div class="say">A relation between two handles, for links too sparse to inline.</div></div>
  </div>
</section>

{"".join(figs_html)}

<section id="index">
  <h2>The full index</h2>
  <div class="tblwrap"><table>
    <caption>All {total} record types, in type_id order within each provider range. The id is the on-disk
    discriminator and never means two things; the name is the stable identity every table, golden and query is
    written against.</caption>
    <thead><tr><th>id</th><th>record</th><th>kind</th><th>what it is</th></tr></thead>
    <tbody>{"".join(rows)}</tbody>
  </table></div>
</section>

<section id="holes">
  <h2>Where the model is still open</h2>
  <p class="sub">Read alongside the figures: these are places a box is missing, not places a box is wrong.</p>
  <div class="holes">
    <div><h3>A protection domain has no record</h3>
      <p><code>ib_queue_pair</code>, <code>ib_mem_region</code>, <code>ib_shared_recv_queue</code> and
      <code>ib_address_handle</code> all carry a <code>pd</code> handle that nothing ever creates. Grouping by it
      works; naming it does not.</p></div>
    <div><h3>Closes cover four openers</h3>
      <p><code>entity_close.opener</code> is a type_id, so the mechanism is open to every provider &mdash; but a close
      naming a subject no open record created is not a bug in the close hook. It is evidence of a creation door
      that is not hooked, and it is counted rather than suppressed.</p></div>
    <div><h3>Three ranges are reserved and empty</h3>
      <p>cupti (300&ndash;399), libfabric (400&ndash;499) and ucx_deep (800&ndash;899) are allocated in
      <code>ids.hpp</code> and have no records. The cupti range names an instrument, which is why
      <code>gpu_kernel</code> deliberately does not live there.</p></div>
    <div><h3>Counter has no producer</h3>
      <p>Of the five temporal kinds, four are in use. Nothing yet emits a Counter &mdash; and <code>gpu_access</code>'s
      Window is a <em>bound</em>, not a measurement, which a renderer that draws it as a duration would silently
      turn into a false claim.</p></div>
  </div>
</section>

<footer>
  This page and the figures it shows are generated by <code>tools/gen_figures.py</code>; edit that, not these.
  Sources: <code>libpcomm/schema/pcomm.fbs</code>, the <code>PCOMM_RECORD</code> declarations in
  <code>libpcomm/</code>, <code>providers/</code> and <code>gpu-backends/</code>, and the view definitions in
  <code>pcomm-read/link_sql.hpp</code>. No webfonts and no scripts: it opens from the filesystem, offline.
</footer>

</div>
</body>
</html>
"""


if __name__ == "__main__":
    import os, sys

    OUT = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "demos", "figures")
    items = [
        ("05-capture-anatomy",  build(), "A capture: per-rank streams, and what a record is"),
        ("06-engine-records",   FIGS["engine"],   "The engine's records, and gpumem's"),
        ("07-ibverbs-records",  FIGS["ib"],       "The ibverbs record family"),
        ("08-mlx5-records",     FIGS["mlx"],      "The mlx5dv / DEVX record family"),
        ("09-ucx-records",      FIGS["ucx"],      "The UCX record family, and ucx_deep beneath it"),
        ("10-nccl-records",     FIGS["nccl"],     "The NCCL record family"),
        ("11-cross-rank-keys",  FIGS["cross"],    "The five cross-rank join keys"),
        ("12-derived-tables",   FIGS["derived"],  "Records to views to the fifteen derived tables"),
    ]
    for name, body, title in items:
        out = convert(body, title)
        assert "var(--" not in out, name
        assert 'class="' not in out, name
        open(f"{OUT}/{name}.svg", "w").write(out)
        print(f"{name}.svg  {len(out):6d} B")

    page = os.path.join(OUT, "..", "entity-map.html")
    open(page, "w").write(page_html())
    print(f"entity-map.html  {os.path.getsize(page):6d} B")
