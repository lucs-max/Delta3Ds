#!/usr/bin/env python3
"""
Builds small synthetic GameMaker data.win files for the Delta3DS test suite.

It contains a tiny assembler for GameMaker bytecode (version 17) and a writer
for the IFF chunks the runner reads. The generated games are original test
programs; no game assets are involved.

    python3 tools/mkdatawin.py out_dir
"""
import struct
import sys
import os

# ---------------------------------------------------------------- opcodes
OP = dict(conv=0x07, mul=0x08, div=0x09, rem=0x0A, mod=0x0B, add=0x0C, sub=0x0D, and_=0x0E, or_=0x0F,
          xor=0x10, neg=0x11, not_=0x12, shl=0x13, shr=0x14, cmp=0x15, pop=0x45, pushi=0x84, dup=0x86,
          callv=0x99, ret=0x9C, exit=0x9D, popz=0x9E, b=0xB6, bt=0xB7, bf=0xB8, pushenv=0xBA, popenv=0xBB,
          push=0xC0, pushloc=0xC1, pushglb=0xC2, pushbltn=0xC3, call=0xD9, brk=0xFF)
T = dict(d=0, f=1, i=2, l=3, b=4, v=5, s=6, e=0xF)
CMP = dict(lt=1, le=2, eq=3, ne=4, ge=5, gt=6)
VT = dict(array=0x00, stacktop=0x80, normal=0xA0, instance=0xE0, pushaf=0x10, popaf=0x90)
SELF, OTHER, ALL, NOONE, GLOBAL, BUILTIN, LOCAL, STACKTOP = -1, -2, -3, -4, -5, -6, -7, -9

BUILTIN_VARS = {"x", "y", "room", "sprite_index", "image_index", "depth", "visible", "id", "object_index",
                "alarm", "image_xscale", "image_yscale", "hspeed", "vspeed", "speed", "direction",
                "argument0", "argument1", "argument2", "argument_count", "argument"}


class Code:
    """Assembler for one code entry."""

    def __init__(self, name, args=0):
        self.name = name
        self.args = args
        self.items = []   # list of (words_placeholder) entries
        self.labels = {}
        self.locals = set()

    # each item: dict(kind=..., ...)
    def _add(self, **kw):
        self.items.append(kw)
        return self

    def label(self, name):
        self.labels[name] = len(self.items)
        return self

    # constants
    def pushi(self, v):
        return self._add(kind="raw", words=[(OP["pushi"] << 24) | (T["e"] << 16) | (v & 0xFFFF)])

    def push_int(self, v):
        return self._add(kind="raw", words=[(OP["push"] << 24) | (T["i"] << 16), v & 0xFFFFFFFF])

    def push_d(self, v):
        lo, hi = struct.unpack("<II", struct.pack("<d", v))
        return self._add(kind="raw", words=[(OP["push"] << 24) | (T["d"] << 16), lo, hi])

    def push_s(self, s):
        return self._add(kind="str", s=s)

    # variables
    def push_var(self, name, inst=SELF, vt="normal", op="push"):
        if inst == LOCAL:
            self.locals.add(name)
        return self._add(kind="var", op=op, name=name, inst=inst, vt=vt)

    def push_glb(self, name):
        return self.push_var(name, GLOBAL, op="pushglb")

    def push_loc(self, name):
        return self.push_var(name, LOCAL, op="pushloc")

    def pop_var(self, name, inst=SELF, vt="normal", src="v"):
        if inst == LOCAL:
            self.locals.add(name)
        return self._add(kind="pop", name=name, inst=inst, vt=vt, src=src)

    def call(self, fn, argc):
        return self._add(kind="call", fn=fn, argc=argc)

    def push_func(self, fn):
        return self._add(kind="funcref", fn=fn)

    # ops
    def op(self, name, t1="v", t2="v"):
        return self._add(kind="raw", words=[(OP[name] << 24) | (T[t2] << 20) | (T[t1] << 16)])

    def conv(self, t1, t2):
        return self.op("conv", t1, t2)

    def cmp(self, kind, t1="v", t2="v"):
        return self._add(kind="raw", words=[(OP["cmp"] << 24) | (T[t2] << 20) | (T[t1] << 16) | (CMP[kind] << 8)])

    def dup(self, n=0, t="v"):
        return self._add(kind="raw", words=[(OP["dup"] << 24) | (T[t] << 16) | n])

    def popz(self, t="v"):
        return self._add(kind="raw", words=[(OP["popz"] << 24) | (T[t] << 16)])

    def ret(self):
        return self._add(kind="raw", words=[(OP["ret"] << 24) | (T["v"] << 16)])

    def exit(self):
        return self._add(kind="raw", words=[(OP["exit"] << 24) | (T["i"] << 16)])

    def jump(self, op, label):
        return self._add(kind="jump", op=op, label=label)

    def brk(self, sub, t="e"):
        return self._add(kind="raw", words=[(OP["brk"] << 24) | (T[t] << 16) | (sub & 0xFFFF)])

    def size(self, item):
        k = item["kind"]
        if k == "raw":
            return len(item["words"])
        if k == "jump":
            return 1
        return 2  # str, var, pop, call, funcref


class Writer:
    def __init__(self):
        self.buf = bytearray()
        self.labels = {}
        self.fix = []  # (pos, label, mode) mode: abs | rel

    def pos(self):
        return len(self.buf)

    def u32(self, v):
        self.buf += struct.pack("<I", v & 0xFFFFFFFF)

    def i32(self, v):
        self.buf += struct.pack("<i", v)

    def f32(self, v):
        self.buf += struct.pack("<f", v)

    def u16(self, v):
        self.buf += struct.pack("<H", v & 0xFFFF)

    def ptr(self, label):
        self.fix.append((self.pos(), label))
        self.u32(0)

    def mark(self, label):
        self.labels[label] = self.pos()

    def patch(self):
        for pos, label in self.fix:
            if label is None:
                continue
            struct.pack_into("<I", self.buf, pos, self.labels[label])


class Game:
    def __init__(self, name="TestGame"):
        self.name = name
        self.strings = []
        self.string_ids = {}
        self.codes = []
        self.objects = []  # dict(name, sprite, events={(type,sub): code_name}, parent)
        self.rooms = []    # dict(name, w, h, insts=[(x,y,obj)], creation=None)
        self.scripts = []  # (name, code_name)
        self.global_init = []
        self.sprites = []  # dict(name, w, h, ox, oy, frames=[tpag index])
        self.tpag = []     # (sx, sy, w, h, page)
        self.pages = []    # PNG bytes

    def s(self, text):
        if text not in self.string_ids:
            self.string_ids[text] = len(self.strings)
            self.strings.append(text)
        return self.string_ids[text]

    def code(self, name, args=0):
        c = Code(name, args)
        self.codes.append(c)
        return c

    def build(self):
        w = Writer()
        w.buf += b"FORM"
        w.u32(0)
        code_index = {c.name: i for i, c in enumerate(self.codes)}
        obj_index = {o["name"]: i for i, o in enumerate(self.objects)}
        for c in self.codes:
            self.s(c.name)

        def chunk(name, body):
            w.buf += name.encode()
            lenpos = w.pos()
            w.u32(0)
            start = w.pos()
            body()
            struct.pack_into("<I", w.buf, lenpos, w.pos() - start)

        def strptr(text):
            self.s(text)
            w.ptr("str:" + text)

        # ---- GEN8
        def gen8():
            w.buf += bytes([0, 17, 0, 0])   # debugger disabled, bytecode 17
            strptr("data")                  # filename
            strptr("Default")               # config
            w.u32(100000 + 1000)            # last obj
            w.u32(10000000)                 # last tile
            w.u32(1234)                     # game id
            w.buf += bytes(16)              # guid
            strptr(self.name)               # name
            for v in (2, 0, 0, 0):          # version
                w.u32(v)
            w.u32(640)
            w.u32(480)
            w.u32(0)                        # info
            w.u32(0)                        # license crc
            w.buf += bytes(16)
            w.buf += bytes(8)               # timestamp
            strptr(self.name)               # display name
            w.buf += bytes(8)               # active targets
            w.buf += bytes(8)               # function classifications
            w.i32(0)                        # steam app id
            w.u32(0)                        # debugger port
            w.u32(len(self.rooms))
            for i in range(len(self.rooms)):
                w.u32(i)
            w.buf += bytes(8 + 32)          # GMS2 random block
            w.f32(30.0)                     # fps
            w.u32(0)                        # allow statistics
            w.buf += bytes(16)
        chunk("GEN8", gen8)

        def empty_list():
            w.u32(0)
        for n in ("SOND", "AGRP"):
            chunk(n, empty_list)

        def sprt():
            w.u32(len(self.sprites))
            for i in range(len(self.sprites)):
                w.ptr("spr:%d" % i)
            for i, sp in enumerate(self.sprites):
                w.mark("spr:%d" % i)
                strptr(sp["name"])
                w.u32(sp["w"]); w.u32(sp["h"])
                w.i32(0); w.i32(sp["w"] - 1); w.i32(sp["h"] - 1); w.i32(0)   # margins l, r, b, t
                w.u32(1); w.u32(0); w.u32(1)                              # transparent, smooth, preload
                w.u32(0); w.u32(0)                                        # bbox mode, sep masks
                w.i32(sp["ox"]); w.i32(sp["oy"])
                w.u32(len(sp["frames"]))
                for t in sp["frames"]:
                    w.ptr("tpag:%d" % t)
        chunk("SPRT", sprt)
        chunk("BGND", empty_list)

        # ---- FONT (empty)
        chunk("FONT", empty_list)

        # ---- OBJT
        def objt():
            w.u32(len(self.objects))
            for i in range(len(self.objects)):
                w.ptr("obj:%d" % i)
            for i, o in enumerate(self.objects):
                w.mark("obj:%d" % i)
                strptr(o["name"])
                w.i32(o.get("sprite", -1))
                w.u32(1)                     # visible
                w.u32(0)                     # solid
                w.i32(o.get("depth", 0))
                w.u32(o.get("persistent", 0))
                parent = o.get("parent")
                w.i32(obj_index[parent] if parent else -100)
                w.i32(-1)                    # mask
                w.u32(0); w.u32(0); w.u32(0)  # physics enabled, sensor, shape
                w.f32(0.5); w.f32(0.1); w.u32(0); w.f32(0.1); w.f32(0.1)
                w.u32(0)                     # vertex count
                w.f32(0.2); w.u32(1); w.u32(0)
                # events: 15 types
                w.u32(15)
                for t in range(15):
                    w.ptr("obj:%d:ev:%d" % (i, t))
                for t in range(15):
                    w.mark("obj:%d:ev:%d" % (i, t))
                    evs = [(sub, cn) for (tt, sub), cn in sorted(o["events"].items()) if tt == t]
                    w.u32(len(evs))
                    for k in range(len(evs)):
                        w.ptr("obj:%d:ev:%d:%d" % (i, t, k))
                    for k, (sub, cn) in enumerate(evs):
                        w.mark("obj:%d:ev:%d:%d" % (i, t, k))
                        w.u32(sub)
                        w.u32(1)
                        w.ptr("obj:%d:ev:%d:%d:a" % (i, t, k))
                        w.mark("obj:%d:ev:%d:%d:a" % (i, t, k))
                        for v in (1, 603, 7, 0, 0, 1, 2):
                            w.u32(v)
                        strptr("")
                        w.i32(code_index[cn])
                        for v in (1, -1, 0, 0, 0):
                            w.i32(v)
        chunk("OBJT", objt)

        # ---- ROOM
        def room():
            w.u32(len(self.rooms))
            for i in range(len(self.rooms)):
                w.ptr("room:%d" % i)
            next_id = 100000
            for i, r in enumerate(self.rooms):
                w.mark("room:%d" % i)
                strptr(r["name"])
                strptr("")
                w.u32(r.get("w", 640)); w.u32(r.get("h", 480)); w.u32(30)
                w.u32(0)                         # persistent
                w.u32(0x000000)                  # bg colour
                w.u32(1)                         # draw bg colour
                cc = r.get("creation")
                w.i32(code_index[cc] if cc else -1)
                w.u32(0)                         # flags (views disabled)
                w.ptr("room:%d:bgs" % i)
                w.ptr("room:%d:views" % i)
                w.ptr("room:%d:objs" % i)
                w.ptr("room:%d:tiles" % i)
                w.u32(1); w.u32(0); w.u32(0); w.u32(1024); w.u32(768)
                w.f32(0); w.f32(10); w.f32(0.1)
                w.ptr("room:%d:layers" % i)
                w.ptr("room:%d:seqs" % i)
                w.mark("room:%d:bgs" % i); w.u32(0)
                w.mark("room:%d:views" % i); w.u32(0)
                w.mark("room:%d:tiles" % i); w.u32(0)
                w.mark("room:%d:seqs" % i); w.u32(0)
                insts = r.get("insts", [])
                w.mark("room:%d:objs" % i)
                w.u32(len(insts))
                for k in range(len(insts)):
                    w.ptr("room:%d:obj:%d" % (i, k))
                ids = []
                for k, (x, y, oname) in enumerate(insts):
                    w.mark("room:%d:obj:%d" % (i, k))
                    w.i32(x); w.i32(y); w.i32(obj_index[oname])
                    w.u32(next_id); ids.append(next_id); next_id += 1
                    w.i32(-1)                    # creation code
                    w.f32(1); w.f32(1); w.f32(1); w.i32(0)
                    w.u32(0xFFFFFFFF); w.f32(0); w.i32(-1)
                # one instance layer holding every instance
                w.mark("room:%d:layers" % i)
                w.u32(1)
                w.ptr("room:%d:layer0" % i)
                w.mark("room:%d:layer0" % i)
                strptr("Instances"); w.u32(1); w.u32(2); w.i32(0)
                w.f32(0); w.f32(0); w.f32(0); w.f32(0); w.u32(1)
                w.u32(len(ids))
                for iid in ids:
                    w.u32(iid)
        chunk("ROOM", room)
        def tpag():
            w.u32(len(self.tpag))
            for i in range(len(self.tpag)):
                w.ptr("tpag:%d" % i)
            for i, (sx, sy, tw, th, page) in enumerate(self.tpag):
                w.mark("tpag:%d" % i)
                for v in (sx, sy, tw, th, 0, 0, tw, th, tw, th):
                    w.u16(v)
                w.u16(page)
        chunk("TPAG", tpag)

        # ---- CODE (assemble)
        var_refs = {}   # (name, inst_kind) -> list of absolute instruction addresses
        func_refs = {}  # name -> list of addresses
        var_order = []
        func_order = []

        def var_key(name, inst):
            kind = "local" if inst == LOCAL else ("global" if inst == GLOBAL else "self")
            k = (name, kind)
            if k not in var_refs:
                var_refs[k] = []
                var_order.append(k)
            return k

        def fn_key(name):
            if name not in func_refs:
                func_refs[name] = []
                func_order.append(name)
            return name

        code_chunk_patches = []  # (pos, key, kind, flags)

        def code():
            w.u32(len(self.codes))
            for i in range(len(self.codes)):
                w.ptr("code:%d" % i)
            # bytecode blobs first
            for i, c in enumerate(self.codes):
                w.mark("blob:%d" % i)
                addr = []
                a = 0
                for it in c.items:
                    addr.append(a)
                    a += c.size(it)
                labels = {name: (addr[idx] if idx < len(addr) else a) for name, idx in c.labels.items()}
                for n, it in enumerate(c.items):
                    k = it["kind"]
                    here = w.pos()
                    if k == "raw":
                        for word in it["words"]:
                            w.u32(word)
                    elif k == "str":
                        w.u32((OP["push"] << 24) | (T["s"] << 16))
                        w.u32(self.s(it["s"]))
                    elif k == "var":
                        inst = it["inst"]
                        op = OP[it["op"]]
                        w.u32((op << 24) | (T["v"] << 16) | (inst & 0xFFFF))
                        key = var_key(it["name"], inst)
                        var_refs[key].append(here)
                        code_chunk_patches.append((w.pos(), VT[it["vt"]]))
                        w.u32(0)
                    elif k == "pop":
                        inst = it["inst"]
                        w.u32((OP["pop"] << 24) | (T[it["src"]] << 20) | (T["v"] << 16) | (inst & 0xFFFF))
                        key = var_key(it["name"], inst)
                        var_refs[key].append(here)
                        code_chunk_patches.append((w.pos(), VT[it["vt"]]))
                        w.u32(0)
                    elif k == "call":
                        w.u32((OP["call"] << 24) | (T["i"] << 16) | it["argc"])
                        func_refs[fn_key(it["fn"])].append(here)
                        w.u32(0)
                    elif k == "funcref":
                        w.u32((OP["push"] << 24) | (T["i"] << 16))
                        func_refs[fn_key(it["fn"])].append(here)
                        w.u32(0)
                    elif k == "jump":
                        off = labels[it["label"]] - addr[n]
                        w.u32((OP[it["op"]] << 24) | (off & 0xFFFFFF))
                w.mark("blobend:%d" % i)
            for i, c in enumerate(self.codes):
                w.mark("code:%d" % i)
                strptr(c.name)
                length = w.labels["blobend:%d" % i] - w.labels["blob:%d" % i]
                w.u32(length)
                w.u16(len(c.locals) + 1)
                w.u16(c.args)
                rel = w.labels["blob:%d" % i] - w.pos()
                w.i32(rel)
                w.u32(0)
        chunk("CODE", code)

        # resolve reference chains: operand = type | next offset; last = name string id
        def write_chain(addrs, name, types):
            for j, a in enumerate(addrs):
                operand_pos = a + 4
                if j + 1 < len(addrs):
                    nxt = addrs[j + 1] - a
                else:
                    nxt = self.s(name)
                t = types.get(operand_pos, 0)
                struct.pack_into("<I", w.buf, operand_pos, ((t & 0xF8) << 24) | (nxt & 0x07FFFFFF))

        types = {pos: t for pos, t in code_chunk_patches}

        def vari():
            w.u32(len(var_order)); w.u32(len(var_order)); w.u32(4)
            for (name, kind) in var_order:
                addrs = var_refs[(name, kind)]
                strptr(name)
                inst = {"local": LOCAL, "global": GLOBAL, "self": SELF}[kind]
                var_id = -6 if (name in BUILTIN_VARS and kind == "self") else 0
                w.i32(inst)
                w.i32(var_id)
                w.u32(len(addrs))
                w.i32(addrs[0] if addrs else -1)
                write_chain(addrs, name, types)
        chunk("VARI", vari)

        def func():
            w.u32(len(func_order))
            for name in func_order:
                addrs = func_refs[name]
                strptr(name)
                w.u32(len(addrs))
                w.i32(addrs[0] if addrs else -1)
                write_chain(addrs, name, {})
            w.u32(0)  # code locals
        chunk("FUNC", func)

        def scpt():
            w.u32(len(self.scripts))
            for i in range(len(self.scripts)):
                w.ptr("scpt:%d" % i)
            for i, (name, cn) in enumerate(self.scripts):
                w.mark("scpt:%d" % i)
                strptr(name)
                w.i32(code_index[cn])
        chunk("SCPT", scpt)

        def glob():
            w.u32(len(self.global_init))
            for cn in self.global_init:
                w.i32(code_index[cn])
        chunk("GLOB", glob)

        def strg():
            w.u32(len(self.strings))
            for i in range(len(self.strings)):
                w.ptr("strent:%d" % i)
            for i, text in enumerate(self.strings):
                w.buf += b"\0" * ((-w.pos()) % 4)
                w.mark("strent:%d" % i)
                data = text.encode("utf-8")
                w.u32(len(data))
                w.labels["str:" + text] = w.pos()
                w.buf += data + b"\0"
        chunk("STRG", strg)

        def txtr():
            w.u32(len(self.pages))
            for i in range(len(self.pages)):
                w.ptr("txtr:%d" % i)
            for i in range(len(self.pages)):
                w.mark("txtr:%d" % i)
                w.u32(0); w.u32(0)
                w.ptr("txtrdata:%d" % i)
            for i, png in enumerate(self.pages):
                w.buf += b"\0" * ((-w.pos()) % 128)
                w.mark("txtrdata:%d" % i)
                w.buf += png
        chunk("TXTR", txtr)

        w.patch()
        struct.pack_into("<I", w.buf, 4, len(w.buf) - 8)
        return bytes(w.buf)


# ---------------------------------------------------------------- test game
def build_test_game():
    g = Game("D3TestGame")

    # global init script: global.counter = 0
    c = g.code("gml_GlobalScript_init")
    c.pushi(0).conv("e", "v").pop_var("counter", GLOBAL, src="v")
    c.push_s("start").pop_var("log", GLOBAL, src="v")
    g.global_init.append(c.name)

    # function add3(a, b, c): return argument0 + argument1 + argument2
    c = g.code("gml_Script_add3", args=3)
    c.push_var("argument0", BUILTIN).push_var("argument1", BUILTIN).op("add").push_var("argument2", BUILTIN).op("add")
    c.ret()
    g.scripts.append(("add3", c.name))

    # obj_controller create
    c = g.code("gml_Object_obj_controller_Create_0")
    # total = add3(1, 2, 3)   (args pushed right to left)
    c.pushi(3).conv("e", "v").pushi(2).conv("e", "v").pushi(1).conv("e", "v").call("add3", 3)
    c.pop_var("total", SELF, src="v")
    c.push_s("total=").push_var("total").call("string", 1).op("add")
    c.call("show_debug_message", 1).popz()
    # arr[0] = 5; arr[2] = 7
    c.pushi(5).conv("e", "v").pushi(SELF).pushi(0).pop_var("arr", SELF, vt="array", src="v")
    c.pushi(7).conv("e", "v").pushi(SELF).pushi(2).pop_var("arr", SELF, vt="array", src="v")
    c.push_s("arrlen=").push_var("arr").call("array_length", 1).call("string", 1).op("add")
    c.call("show_debug_message", 1).popz()
    c.push_s("arr2=").pushi(SELF).pushi(2).push_var("arr", SELF, vt="array").call("string", 1).op("add")
    c.call("show_debug_message", 1).popz()
    # local loop: var i = 0, sum = 0; while (i < 5) { sum += i; i++ }
    c.pushi(0).conv("e", "v").pop_var("i", LOCAL)
    c.pushi(0).conv("e", "v").pop_var("sum", LOCAL)
    c.label("loop")
    c.push_loc("i").pushi(5).conv("e", "v").cmp("lt").jump("bf", "endloop")
    c.push_loc("sum").push_loc("i").op("add").pop_var("sum", LOCAL)
    c.push_loc("i").pushi(1).conv("e", "v").op("add").pop_var("i", LOCAL)
    c.jump("b", "loop")
    c.label("endloop")
    c.push_s("sum=").push_loc("sum").call("string", 1).op("add").call("show_debug_message", 1).popz()
    # string functions
    c.push_s("upper=").push_s("hello").call("string_upper", 1).op("add").call("show_debug_message", 1).popz()
    c.push_s("len=").push_s("deltarune3ds").call("string_length", 1).call("string", 1).op("add")
    c.call("show_debug_message", 1).popz()
    # real division and integer div
    c.push_s("div=").pushi(7).conv("e", "v").pushi(2).conv("e", "v").op("div").call("string", 1).op("add")
    c.call("show_debug_message", 1).popz()
    # create two children
    for _ in range(2):
        c.pushi(1).conv("e", "v")          # obj_child index (arg3)
        c.pushi(0).conv("e", "v")          # depth
        c.pushi(0).conv("e", "v")          # y
        c.pushi(0).conv("e", "v")          # x
        c.push_func("instance_create_depth") if False else None
        c.call("instance_create_depth", 4).popz()
    c.push_s("children=").pushi(1).conv("e", "v").call("instance_number", 1).call("string", 1).op("add")
    c.call("show_debug_message", 1).popz()
    # with (obj_child) v = 42
    c.pushi(1).conv("e", "v").jump("pushenv", "withend")
    c.label("withbody")
    c.pushi(42).conv("e", "v").pop_var("v", SELF)
    c.label("withend")
    c.jump("popenv", "withbody")
    # struct literal via constructor: s = {a: 9}
    c.push_func("gml_Script___struct___1").conv("i", "v").call("@@NewGMLObject@@", 1)
    c.pop_var("st", SELF)
    c.push_s("struct.a=").push_var("st").pop_var("tmp", LOCAL)
    c.push_loc("tmp").push_var("a", STACKTOP, vt="stacktop").call("string", 1).op("add")
    c.call("show_debug_message", 1).popz()
    c.exit()

    # struct constructor
    c = g.code("gml_Script___struct___1")
    c.pushi(9).conv("e", "v").pop_var("a", SELF)
    c.exit()

    # obj_controller step: global.counter += 1; if (global.counter == 3) room_goto(1)
    c = g.code("gml_Object_obj_controller_Step_0")
    c.push_glb("counter").pushi(1).conv("e", "v").op("add").pop_var("counter", GLOBAL)
    c.push_glb("counter").pushi(3).conv("e", "v").cmp("eq").jump("bf", "skip")
    c.pushi(1).conv("e", "v").call("room_goto", 1).popz()
    c.label("skip")
    c.exit()

    # obj_child create: v = 0 ; draw: draw_rectangle(...)
    c = g.code("gml_Object_obj_child_Create_0")
    c.pushi(0).conv("e", "v").pop_var("v", SELF)
    c.exit()

    # obj_marker in room_b reports the children's values on creation
    c = g.code("gml_Object_obj_marker_Create_0")
    c.push_s("room_b reached, counter=").push_glb("counter").call("string", 1).op("add")
    c.call("show_debug_message", 1).popz()
    c.exit()

    # obj_controller draw: draw_text(10, 10, "hi")
    c = g.code("gml_Object_obj_controller_Draw_0")
    c.push_s("hi").pushi(10).conv("e", "v").pushi(10).conv("e", "v").call("draw_text", 3).popz()
    c.pushi(0).conv("e", "v").pushi(20).conv("e", "v").pushi(20).conv("e", "v").pushi(0).conv("e", "v")
    c.pushi(0).conv("e", "v").call("draw_rectangle", 5).popz()
    c.exit()

    # obj_child step: check v was set by the with statement (once)
    c = g.code("gml_Object_obj_child_Step_0")
    c.push_s("child v=").push_var("v").call("string", 1).op("add").call("show_debug_message", 1).popz()
    c.exit()

    g.objects = [
        dict(name="obj_controller", events={(0, 0): "gml_Object_obj_controller_Create_0",
                                            (3, 0): "gml_Object_obj_controller_Step_0",
                                            (8, 0): "gml_Object_obj_controller_Draw_0"}, persistent=1),
        dict(name="obj_child", events={(0, 0): "gml_Object_obj_child_Create_0",
                                       (3, 0): "gml_Object_obj_child_Step_0"}),
        dict(name="obj_marker", events={(0, 0): "gml_Object_obj_marker_Create_0"}),
    ]
    g.rooms = [
        dict(name="room_a", insts=[(0, 0, "obj_controller")]),
        dict(name="room_b", insts=[(5, 5, "obj_marker")]),
    ]
    return g.build()


def build_sprite_game():
    """A one-room game that draws a two-frame sprite (plain coloured squares)."""
    import io
    from PIL import Image, ImageDraw
    g = Game("D3SpriteTest")
    page = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(page)
    d.rectangle((0, 0, 15, 15), fill=(255, 0, 0, 255))
    d.rectangle((16, 0, 31, 15), fill=(0, 0, 255, 255))
    buf = io.BytesIO()
    page.save(buf, "PNG")
    g.pages.append(buf.getvalue())
    g.tpag = [(0, 0, 16, 16, 0), (16, 0, 16, 16, 0)]
    g.sprites = [dict(name="spr_square", w=16, h=16, ox=8, oy=8, frames=[0, 1])]
    c = g.code("gml_Object_obj_box_Create_0")
    c.pushi(0).conv("e", "v").pop_var("sprite_index", SELF)
    c.pushi(2).conv("e", "v").pop_var("image_xscale", SELF)
    c.push_s("frames=").pushi(0).conv("e", "v").call("sprite_get_number", 1).call("string", 1).op("add")
    c.call("show_debug_message", 1).popz()
    c.exit()
    g.objects = [dict(name="obj_box", events={(0, 0): c.name})]
    g.rooms = [dict(name="room_draw", insts=[(100, 100, "obj_box")])]
    return g.build()


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "data.win")
    with open(path, "wb") as f:
        f.write(build_test_game())
    print(path)
    sprite_dir = os.path.join(out, "sprite_game")
    os.makedirs(sprite_dir, exist_ok=True)
    with open(os.path.join(sprite_dir, "data.win"), "wb") as f:
        f.write(build_sprite_game())
    print(os.path.join(sprite_dir, "data.win"))
