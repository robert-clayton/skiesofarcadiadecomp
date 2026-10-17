"""PowerPC -> C emitter.

Every recovered function becomes one C function of the shape

    void fn_800A1234(CpuState* s) { ... }

operating directly on the guest state (runtime/cpu.h). Within a function,
basic blocks are labels and direct branches are ``goto``; a ``bl`` is a call
to the target's C function; ``blr`` is ``return``; a resolved switch table is
a ``switch`` over the case addresses; an indirect call goes through the
runtime's ``dispatch``.

Instructions the emitter does not handle yet are emitted as calls to
``guest_unimplemented`` and counted, so coverage is measured rather than
assumed. Paired singles land here for now (slice 3.4).
"""

import collections
from dataclasses import dataclass, field

from ..ppc import isa
from ..ppc.cfg import CodeView, Function, JumpTable
from ..ppc.decode import Insn


def c_name(addr: int) -> str:
    return f"fn_{addr:08X}"


def label(addr: int) -> str:
    return f"L_{addr:08X}"


def u32(v: int) -> str:
    return f"0x{v & 0xFFFFFFFF:08X}u"


def ppc_mask(mb: int, me: int) -> int:
    """The rotate-and-mask family's mask, MSB-numbered and wrapping when mb > me."""
    lo = 0xFFFFFFFF >> mb
    hi = (0xFFFFFFFF << (31 - me)) & 0xFFFFFFFF
    return (lo & hi) if mb <= me else (lo | hi)


def crm_mask(crm: int) -> int:
    """Expand an 8-bit CR field mask to a 32-bit bit mask."""
    m = 0
    for i in range(8):
        if crm & (0x80 >> i):
            m |= 0xF << (28 - 4 * i)
    return m


@dataclass
class EmitStats:
    translated: collections.Counter = field(default_factory=collections.Counter)
    unsupported: collections.Counter = field(default_factory=collections.Counter)
    oe_ignored: int = 0

    @property
    def total(self) -> int:
        return sum(self.translated.values()) + sum(self.unsupported.values())

    @property
    def coverage_pct(self) -> float:
        t = self.total
        return 100.0 * sum(self.translated.values()) / t if t else 0.0


# SPRs with dedicated fields in CpuState.
_SPR_FIELDS = {1: "s->xer", 8: "s->lr", 9: "s->ctr", 22: "s->dec", 920: "s->hid2", 921: "s->wpar"}

_NOP_MNEMONICS = frozenset(
    ["sync", "isync", "eieio", "dcbf", "dcbst", "dcbi", "dcbt", "dcbtst", "icbi", "tlbsync"]
)

_LOGICAL = {
    "and": "{a} & {b}",
    "andc": "{a} & ~{b}",
    "or": "{a} | {b}",
    "orc": "{a} | ~{b}",
    "xor": "{a} ^ {b}",
    "nand": "~({a} & {b})",
    "nor": "~({a} | {b})",
    "eqv": "~({a} ^ {b})",
}

_CR_OPS = {
    "crand": "{a} & {b}",
    "crandc": "{a} & !{b}",
    "cror": "{a} | {b}",
    "crorc": "{a} | !{b}",
    "crxor": "{a} ^ {b}",
    "crnand": "!({a} & {b})",
    "crnor": "!({a} | {b})",
    "creqv": "!({a} ^ {b})",
}

# Loads: mnemonic -> (C expression producing the value, update-form?)
_LOADS = {
    "lwz": ("mem_r32(s, {ea})", False),
    "lwzu": ("mem_r32(s, {ea})", True),
    "lbz": ("(uint32_t)mem_r8(s, {ea})", False),
    "lbzu": ("(uint32_t)mem_r8(s, {ea})", True),
    "lhz": ("(uint32_t)mem_r16(s, {ea})", False),
    "lhzu": ("(uint32_t)mem_r16(s, {ea})", True),
    "lha": ("(uint32_t)(int32_t)(int16_t)mem_r16(s, {ea})", False),
    "lhau": ("(uint32_t)(int32_t)(int16_t)mem_r16(s, {ea})", True),
    "lwzx": ("mem_r32(s, {ea})", False),
    "lwzux": ("mem_r32(s, {ea})", True),
    "lbzx": ("(uint32_t)mem_r8(s, {ea})", False),
    "lbzux": ("(uint32_t)mem_r8(s, {ea})", True),
    "lhzx": ("(uint32_t)mem_r16(s, {ea})", False),
    "lhzux": ("(uint32_t)mem_r16(s, {ea})", True),
    "lhax": ("(uint32_t)(int32_t)(int16_t)mem_r16(s, {ea})", False),
    "lhaux": ("(uint32_t)(int32_t)(int16_t)mem_r16(s, {ea})", True),
    "lwbrx": ("BSWAP32(mem_r32(s, {ea}))", False),
    "lhbrx": ("(uint32_t)BSWAP16(mem_r16(s, {ea}))", False),
    "lwarx": ("mem_r32(s, {ea})", False),
}

# Stores: mnemonic -> (C statement template, update-form?)
_STORES = {
    "stw": ("mem_w32(s, {ea}, {v});", False),
    "stwu": ("mem_w32(s, {ea}, {v});", True),
    "stb": ("mem_w8(s, {ea}, (uint8_t){v});", False),
    "stbu": ("mem_w8(s, {ea}, (uint8_t){v});", True),
    "sth": ("mem_w16(s, {ea}, (uint16_t){v});", False),
    "sthu": ("mem_w16(s, {ea}, (uint16_t){v});", True),
    "stwx": ("mem_w32(s, {ea}, {v});", False),
    "stwux": ("mem_w32(s, {ea}, {v});", True),
    "stbx": ("mem_w8(s, {ea}, (uint8_t){v});", False),
    "stbux": ("mem_w8(s, {ea}, (uint8_t){v});", True),
    "sthx": ("mem_w16(s, {ea}, (uint16_t){v});", False),
    "sthux": ("mem_w16(s, {ea}, (uint16_t){v});", True),
    "stwbrx": ("mem_w32(s, {ea}, BSWAP32({v}));", False),
    "sthbrx": ("mem_w16(s, {ea}, BSWAP16((uint16_t){v}));", False),
}

# Float arithmetic, single (fills both halves) and double (ps0 only).
# Fused forms use fma() so they round once, as the hardware does.
_FLOAT_ARITH = {
    "fadds": ("{a} + {b}", True),
    "fsubs": ("{a} - {b}", True),
    "fmuls": ("{a} * {c}", True),
    "fdivs": ("{a} / {b}", True),
    "fmadds": ("fma({a}, {c}, {b})", True),
    "fmsubs": ("fma({a}, {c}, -{b})", True),
    "fnmadds": ("-fma({a}, {c}, {b})", True),
    "fnmsubs": ("-fma({a}, {c}, -{b})", True),
    "fres": ("(double)(1.0f / (float){b})", True),
    "fadd": ("{a} + {b}", False),
    "fsub": ("{a} - {b}", False),
    "fmul": ("{a} * {c}", False),
    "fdiv": ("{a} / {b}", False),
    "fmadd": ("fma({a}, {c}, {b})", False),
    "fmsub": ("fma({a}, {c}, -{b})", False),
    "fnmadd": ("-fma({a}, {c}, {b})", False),
    "fnmsub": ("-fma({a}, {c}, -{b})", False),
    "frsqrte": ("1.0 / sqrt({b})", False),
    "fsel": ("({a} >= 0.0) ? {c} : {b}", False),
}


# Paired singles: (ps0 expression, ps1 expression). Every result is rounded
# to single precision, because that is what a paired single is. Inputs are
# read into temporaries first since rD may alias a source.
_PS_PAIR = {
    "ps_add": ("{a0} + {b0}", "{a1} + {b1}"),
    "ps_sub": ("{a0} - {b0}", "{a1} - {b1}"),
    "ps_mul": ("{a0} * {c0}", "{a1} * {c1}"),
    "ps_div": ("{a0} / {b0}", "{a1} / {b1}"),
    "ps_madd": ("fma({a0}, {c0}, {b0})", "fma({a1}, {c1}, {b1})"),
    "ps_msub": ("fma({a0}, {c0}, -{b0})", "fma({a1}, {c1}, -{b1})"),
    "ps_nmadd": ("-fma({a0}, {c0}, {b0})", "-fma({a1}, {c1}, {b1})"),
    "ps_nmsub": ("-fma({a0}, {c0}, -{b0})", "-fma({a1}, {c1}, -{b1})"),
    "ps_muls0": ("{a0} * {c0}", "{a1} * {c0}"),
    "ps_muls1": ("{a0} * {c1}", "{a1} * {c1}"),
    "ps_madds0": ("fma({a0}, {c0}, {b0})", "fma({a1}, {c0}, {b1})"),
    "ps_madds1": ("fma({a0}, {c1}, {b0})", "fma({a1}, {c1}, {b1})"),
    "ps_sum0": ("{a0} + {b1}", "{c1}"),
    "ps_sum1": ("{c0}", "{a0} + {b1}"),
    "ps_neg": ("-{b0}", "-{b1}"),
    "ps_abs": ("fabs({b0})", "fabs({b1})"),
    "ps_nabs": ("-fabs({b0})", "-fabs({b1})"),
    "ps_mr": ("{b0}", "{b1}"),
    "ps_res": ("1.0 / {b0}", "1.0 / {b1}"),
    "ps_rsqrte": ("1.0 / sqrt({b0})", "1.0 / sqrt({b1})"),
    "ps_sel": ("({a0} >= 0.0) ? {c0} : {b0}", "({a1} >= 0.0) ? {c1} : {b1}"),
    "ps_merge00": ("{a0}", "{b0}"),
    "ps_merge01": ("{a0}", "{b1}"),
    "ps_merge10": ("{a1}", "{b0}"),
    "ps_merge11": ("{a1}", "{b1}"),
}

# The guard at the head of every translated function (docs/SWAP.md): one
# byte read and a branch never taken until runtime/swap.c sets the byte, to
# send the call to a replacement, or to count it. soa_swap_enter answers 1
# when the call has been answered (the translation is not to run), 0 when the
# translation is to run as if nothing had asked. The byte is read volatile:
# the host sets it from its own thread while the guest runs.
SWAP_GUARD = """#ifndef SOA_SWAP_HIDDEN
#if defined(_MSC_VER)
#define SOA_SWAP_HIDDEN
#else
#define SOA_SWAP_HIDDEN __attribute__((visibility("hidden")))
#endif
#endif
#if defined(__GNUC__) || defined(__clang__)
#define SOA_SWAP_RARE(x) __builtin_expect(!!(x), 0)
#else
#define SOA_SWAP_RARE(x) (x)
#endif
extern SOA_SWAP_HIDDEN uint8_t soa_swap_flags[];
int soa_swap_enter(CpuState* s, uint32_t index); /* runtime/swap.c */
#define SOA_SWAP(i) \\
    do { \\
        if (SOA_SWAP_RARE(((volatile uint8_t*)soa_swap_flags)[i]) && soa_swap_enter(s, (i))) return; \\
    } while (0)"""

_PSQ = frozenset(
    ["psq_l", "psq_lu", "psq_lx", "psq_lux", "psq_st", "psq_stu", "psq_stx", "psq_stux"]
)


class Emitter:
    def __init__(
        self,
        dol,
        functions: dict[int, Function],
        tables: dict[int, JumpTable],
        code: CodeView | None = None,
        names: dict[int, str] | None = None,
        hle: dict[int, str] | None = None,
        hooks: dict[int, str] | None = None,
        savepoints: dict[int, str] | None = None,
        traces: dict[int, str] | None = None,
        swap: bool = False,
    ):
        self.dol = dol
        self.functions = functions
        self.tables = tables
        self.code = code or CodeView(dol)
        self.names = names or {}
        self.hle = hle or {}  # functions the runtime provides natively
        self.hooks = hooks or {}  # addresses where the runtime is called first
        self.savepoints = savepoints or {}  # call sites wrapped in setjmp (thread parking)
        self.traces = traces or {}  # addresses that report themselves when SOA_TRACE is set
        # swap: every translated function's entry is guarded by SOA_SWAP(index)
        # (runtime/swap.c, docs/SWAP.md), the index its place in address order,
        # which is also the swap table's (seam.swap_table_c). Off by default,
        # so a lone function compiles against cpu.h alone.
        self.swap = swap
        self.index = {a: n for n, a in enumerate(sorted(functions))}
        self.stats = EmitStats()
        self._hooked = False  # the function being emitted delivers interrupts at a hook

    # ------------------------------------------------------------------ API

    def function_c(self, fn: Function) -> str:
        addrs = sorted({a for b in fn.blocks.values() for a in range(b.start, b.end, 4)})
        addr_set = set(addrs)

        labels = set(fn.blocks)
        for a in addrs:
            i = self.code.at(a)
            if i and i.valid and i.is_direct_branch and not i.lk_bit and i.target in addr_set:
                labels.add(i.target)
            if i and i.valid and i.mnemonic == "bcctr" and a in self.tables:
                labels.update(t for t in self.tables[a].targets if t in addr_set)

        self._hooked = any(a in self.hooks for a in addrs)
        pretty = self.names.get(fn.entry)
        name = c_name(fn.entry)
        head = f"/* {pretty} */\n" if pretty and pretty != name else ""
        if fn.entry in self.hle:
            # The runtime defines fn_X; keep the recompiled body under another
            # name so the two can be run against each other.
            head = f"/* {self.hle[fn.entry]} -- bound to HLE; recompiled body kept */\n"
            name = "recomp_" + name
        out = [f"{head}void {name}(CpuState* s)", "{"]
        if self.swap and fn.entry not in self.hle:
            # First, before the entry's own label: a loop back to the entry
            # is not a second call.
            out.append(f"    SOA_SWAP({self.index[fn.entry]});")
        for a in addrs:
            if a in labels:
                out.append(f"{label(a)}:;")
                # Block-level position, so a hang or trap can say where it is.
                out.append(f"    s->pc = {u32(a)};")
            i = self.code.at(a)
            out.append(f"    /* {a:08X} {i.mnemonic if i and i.valid else '??'} */")
            if a in self.hooks:
                out.append(f"    hook_{a:08X}(s); /* {self.hooks[a]} */")
            if a in self.traces:
                out.append(f'    trace_hit(s, {u32(a)}, "{self.traces[a]}");')
            for st in self._translate(i, addr_set):
                out.append("    " + st)
        out.append("    return;")
        out.append("}")
        return "\n".join(out)

    def prototypes(self) -> str:
        lines = ["#pragma once", '#include "cpu.h"', ""]
        if self.swap:
            lines += SWAP_GUARD.splitlines() + [""]
        for a in sorted(self.functions):
            lines.append(f"void {c_name(a)}(CpuState* s);")
            if a in self.hle:
                lines.append(f"void recomp_{c_name(a)}(CpuState* s); /* {self.hle[a]}: HLE */")
        for a, name in sorted(self.hooks.items()):
            lines.append(f"void hook_{a:08X}(CpuState* s); /* {name} */")
        return "\n".join(lines) + "\n"

    def dispatch_c(self) -> str:
        lines = ['#include "functions.h"', "", "void dispatch(CpuState* s, uint32_t addr)", "{"]
        lines.append("    switch (addr) {")
        for a in sorted(self.functions):
            lines.append(f"    case {u32(a)}: {c_name(a)}(s); return;")
        lines.append("    default: guest_trap(s, addr); return;")
        lines.append("    }")
        lines.append("}")
        # The same list, as a question: a native caller that takes an address
        # from outside (runtime/mod.c's call_guest) asks before dispatching,
        # since an unknown address is a trap that ends the run.
        lines += ["", "int dispatch_known(uint32_t addr)", "{", "    switch (addr) {"]
        for a in sorted(self.functions):
            lines.append(f"    case {u32(a)}:")
        lines += ["        return 1;", "    default:", "        return 0;", "    }", "}"]
        return "\n".join(lines) + "\n"

    # ------------------------------------------------------------ helpers

    def _unsupported(self, i: Insn, why: str | None = None) -> list[str]:
        self.stats.unsupported[i.mnemonic if i.valid else "invalid"] += 1
        what = why or i.mnemonic
        return [f'guest_unimplemented(s, {u32(i.addr)}, "{what}");']

    def _call(self, target: int, next_pc: int) -> str:
        """A call that returns here: set LR, then call the target directly if known."""
        if target in self.functions:
            return f"s->lr = {u32(next_pc)}; {c_name(target)}(s);"
        return f"s->lr = {u32(next_pc)}; dispatch(s, {u32(target)});"

    def _jump(self, target: int, addr_set: set[int], pc: int) -> str:
        """An unconditional transfer: local goto, tail call, or trap.

        A backward goto is a loop edge, and loops are where a thread waits
        for something an interrupt handler does -- so each one polls for
        pending interrupts first (runtime/irq.c). Functions with a hook
        deliver at the hook instead; the scheduler's idle loop is one.
        """
        if target in addr_set:
            poll = "irq_poll(s); " if target <= pc and not self._hooked else ""
            return f"{poll}goto {label(target)};"
        if target in self.functions:
            return f"{c_name(target)}(s); return;"
        return f"guest_trap(s, {u32(pc)}); return;"

    @staticmethod
    def _cond(bo: int, bi: int) -> str:
        parts = []
        if not (bo & 16):
            want = (bo >> 3) & 1
            parts.append(f"cr_bit(s, {bi}) == {want}")
        if not (bo & 4):
            parts.append("s->ctr == 0" if (bo & 2) else "s->ctr != 0")
        return " && ".join(f"({p})" for p in parts) if parts else "1"

    # ---------------------------------------------------------- translate

    def _translate(self, i: Insn, addr_set: set[int]) -> list[str]:  # noqa: PLR0911, PLR0912, PLR0915
        if i is None or not i.valid:
            return self._unsupported(i, "invalid")

        m = i.mnemonic
        pc, nxt = i.addr, i.addr + 4
        rd, ra, rb, rc = i.rd, i.ra, i.rb, i.rc

        def G(n: int) -> str:
            return f"s->gpr[{n}]"

        def GA(n: int) -> str:
            return "0u" if n == 0 else G(n)

        def F(n: int) -> str:
            return f"s->fpr[{n}].ps0"

        def F1(n: int) -> str:
            return f"s->fpr[{n}].ps1"

        st: list[str] = []

        def rc_(expr: str) -> None:
            if i.rc_bit:
                st.append(f"CR0_RC(s, {expr});")

        if i.oe_bit:
            self.stats.oe_ignored += 1

        self.stats.translated[m] += 1  # optimistic; undone on the unsupported path

        # ---- immediates -----------------------------------------------
        if m == "addi":
            st.append(f"{G(rd)} = {GA(ra)} + {u32(i.imm)};")
        elif m == "addis":
            st.append(f"{G(rd)} = {GA(ra)} + {u32(i.imm << 16)};")
        elif m == "addic":
            st.append(f"{G(rd)} = ppc_addc(s, {G(ra)}, {u32(i.imm)});")
        elif m == "addic.":
            st.append(f"{G(rd)} = ppc_addc(s, {G(ra)}, {u32(i.imm)});")
            st.append(f"CR0_RC(s, {G(rd)});")
        elif m == "subfic":
            st.append(f"{G(rd)} = ppc_subfc(s, {G(ra)}, {u32(i.imm)});")
        elif m == "mulli":
            st.append(f"{G(rd)} = {G(ra)} * {u32(i.imm)};")
        elif m in ("ori", "xori"):
            op = "|" if m == "ori" else "^"
            st.append(f"{G(ra)} = {G(rd)} {op} {u32(i.imm)};")
        elif m in ("oris", "xoris"):
            op = "|" if m == "oris" else "^"
            st.append(f"{G(ra)} = {G(rd)} {op} {u32(i.imm << 16)};")
        elif m == "andi.":
            st.append(f"{G(ra)} = {G(rd)} & {u32(i.imm)};")
            st.append(f"CR0_RC(s, {G(ra)});")
        elif m == "andis.":
            st.append(f"{G(ra)} = {G(rd)} & {u32(i.imm << 16)};")
            st.append(f"CR0_RC(s, {G(ra)});")
        elif m == "cmpi":
            st.append(f"cr_cmp_s(s, {i.crf_d}, (int32_t){G(ra)}, (int32_t){u32(i.imm)});")
        elif m == "cmpli":
            st.append(f"cr_cmp_u(s, {i.crf_d}, {G(ra)}, {u32(i.imm)});")
        elif m == "cmp":
            st.append(f"cr_cmp_s(s, {i.crf_d}, (int32_t){G(ra)}, (int32_t){G(rb)});")
        elif m == "cmpl":
            st.append(f"cr_cmp_u(s, {i.crf_d}, {G(ra)}, {G(rb)});")

        # ---- loads / stores -------------------------------------------
        elif m in _LOADS:
            expr, update = _LOADS[m]
            ea = f"({GA(ra)} + {G(rb)})" if m.endswith("x") else f"({GA(ra)} + {u32(i.imm)})"
            if update:
                st.append(
                    f"{{ uint32_t ea = {ea}; {G(rd)} = {expr.format(ea='ea')}; {G(ra)} = ea; }}"
                )
            else:
                st.append(f"{G(rd)} = {expr.format(ea=ea)};")
        elif m in _STORES:
            tmpl, update = _STORES[m]
            ea = f"({GA(ra)} + {G(rb)})" if m.endswith("x") else f"({GA(ra)} + {u32(i.imm)})"
            if update:
                st.append(
                    f"{{ uint32_t ea = {ea}; {tmpl.format(ea='ea', v=G(rd))} {G(ra)} = ea; }}"
                )
            else:
                st.append(tmpl.format(ea=ea, v=G(rd)))
        elif m == "stwcx.":
            st.append(f"mem_w32(s, ({GA(ra)} + {G(rb)}), {G(rd)});")
            st.append("cr_set_field(s, 0, 2u | ((s->xer & XER_SO) ? 1u : 0u));")
        elif m == "lmw":
            st.append(f"ppc_lmw(s, {rd}, {GA(ra)} + {u32(i.imm)});")
        elif m == "stmw":
            st.append(f"ppc_stmw(s, {rd}, {GA(ra)} + {u32(i.imm)});")
        elif m in ("lfs", "lfsu", "lfsx", "lfsux"):
            ea = f"({GA(ra)} + {G(rb)})" if m.endswith("x") else f"({GA(ra)} + {u32(i.imm)})"
            body = f"{F(rd)} = s->fpr[{rd}].ps1 = (double)mem_rf32(s, {{ea}});"
            if m in ("lfsu", "lfsux"):
                st.append(f"{{ uint32_t ea = {ea}; {body.format(ea='ea')} {G(ra)} = ea; }}")
            else:
                st.append(body.format(ea=ea))
        elif m in ("lfd", "lfdu", "lfdx", "lfdux"):
            ea = f"({GA(ra)} + {G(rb)})" if m.endswith("x") else f"({GA(ra)} + {u32(i.imm)})"
            body = f"{F(rd)} = mem_rf64(s, {{ea}});"
            if m in ("lfdu", "lfdux"):
                st.append(f"{{ uint32_t ea = {ea}; {body.format(ea='ea')} {G(ra)} = ea; }}")
            else:
                st.append(body.format(ea=ea))
        elif m in ("stfs", "stfsu", "stfsx", "stfsux"):
            ea = f"({GA(ra)} + {G(rb)})" if m.endswith("x") else f"({GA(ra)} + {u32(i.imm)})"
            body = f"mem_wf32(s, {{ea}}, (float){F(rd)});"
            if m in ("stfsu", "stfsux"):
                st.append(f"{{ uint32_t ea = {ea}; {body.format(ea='ea')} {G(ra)} = ea; }}")
            else:
                st.append(body.format(ea=ea))
        elif m in ("stfd", "stfdu", "stfdx", "stfdux"):
            ea = f"({GA(ra)} + {G(rb)})" if m.endswith("x") else f"({GA(ra)} + {u32(i.imm)})"
            body = f"mem_wf64(s, {{ea}}, {F(rd)});"
            if m in ("stfdu", "stfdux"):
                st.append(f"{{ uint32_t ea = {ea}; {body.format(ea='ea')} {G(ra)} = ea; }}")
            else:
                st.append(body.format(ea=ea))
        elif m == "stfiwx":
            st.append(f"mem_w32(s, ({GA(ra)} + {G(rb)}), (uint32_t)fpr_bits(s, {rd}));")
        elif m in ("dcbz", "dcbz_l"):
            st.append(f"mem_zero32(s, {GA(ra)} + {G(rb)});")

        # ---- integer arithmetic (XO) ----------------------------------
        elif m == "add":
            st.append(f"{G(rd)} = {G(ra)} + {G(rb)};")
            rc_(G(rd))
        elif m == "subf":
            st.append(f"{G(rd)} = {G(rb)} - {G(ra)};")
            rc_(G(rd))
        elif m == "neg":
            st.append(f"{G(rd)} = 0u - {G(ra)};")
            rc_(G(rd))
        elif m in ("addc", "adde", "subfc", "subfe"):
            st.append(f"{G(rd)} = ppc_{m}(s, {G(ra)}, {G(rb)});")
            rc_(G(rd))
        elif m in ("addze", "subfze", "addme", "subfme"):
            st.append(f"{G(rd)} = ppc_{m}(s, {G(ra)});")
            rc_(G(rd))
        elif m == "mullw":
            st.append(f"{G(rd)} = {G(ra)} * {G(rb)};")
            rc_(G(rd))
        elif m in ("mulhw", "mulhwu", "divw", "divwu"):
            st.append(f"{G(rd)} = ppc_{m}({G(ra)}, {G(rb)});")
            rc_(G(rd))

        # ---- logical / shift / rotate ---------------------------------
        elif m in _LOGICAL:
            st.append(f"{G(ra)} = {_LOGICAL[m].format(a=G(rd), b=G(rb))};")
            rc_(G(ra))
        elif m in ("slw", "srw"):
            st.append(f"{G(ra)} = ppc_{m}({G(rd)}, {G(rb)});")
            rc_(G(ra))
        elif m == "sraw":
            st.append(f"{G(ra)} = ppc_sraw(s, {G(rd)}, {G(rb)});")
            rc_(G(ra))
        elif m == "srawi":
            st.append(f"{G(ra)} = ppc_srawi(s, {G(rd)}, {i.imm});")
            rc_(G(ra))
        elif m == "cntlzw":
            st.append(f"{G(ra)} = ppc_cntlzw({G(rd)});")
            rc_(G(ra))
        elif m == "extsb":
            st.append(f"{G(ra)} = (uint32_t)(int32_t)(int8_t){G(rd)};")
            rc_(G(ra))
        elif m == "extsh":
            st.append(f"{G(ra)} = (uint32_t)(int32_t)(int16_t){G(rd)};")
            rc_(G(ra))
        elif m == "rlwinm":
            mask = ppc_mask(i.mb, i.me)
            rot = f"rotl32({G(rd)}, {i.imm})" if i.imm else G(rd)
            st.append(f"{G(ra)} = {rot} & {u32(mask)};")
            rc_(G(ra))
        elif m == "rlwimi":
            mask = ppc_mask(i.mb, i.me)
            rot = f"rotl32({G(rd)}, {i.imm})" if i.imm else G(rd)
            st.append(f"{G(ra)} = ({rot} & {u32(mask)}) | ({G(ra)} & {u32(~mask)});")
            rc_(G(ra))
        elif m == "rlwnm":
            mask = ppc_mask(i.mb, i.me)
            st.append(f"{G(ra)} = rotl32({G(rd)}, {G(rb)} & 31u) & {u32(mask)};")
            rc_(G(ra))

        # ---- special registers / CR -----------------------------------
        elif m == "mfspr" and i.spr == 22:
            st.append(f"{G(rd)} = dec_read(s);")
        elif m == "mtspr" and i.spr == 22:
            st.append(f"dec_write(s, {G(rd)});")
        elif m == "mfspr":
            src = _SPR_FIELDS.get(i.spr)
            if src is None and i.spr in isa.GQR_SPRS:
                src = f"s->gqr[{i.spr - 912}]"
            st.append(f"{G(rd)} = {src or f's->spr[{i.spr}]'};")
        elif m == "mtspr":
            dst = _SPR_FIELDS.get(i.spr)
            if dst is None and i.spr in isa.GQR_SPRS:
                dst = f"s->gqr[{i.spr - 912}]"
            st.append(f"{dst or f's->spr[{i.spr}]'} = {G(rd)};")
        elif m == "mftb":
            st.append(f"{G(rd)} = guest_timebase_lo(s);")
        elif m == "mftbu":
            st.append(f"{G(rd)} = guest_timebase_hi(s);")
        elif m == "mfcr":
            st.append(f"{G(rd)} = s->cr;")
        elif m == "mtcrf":
            mask = crm_mask(i.imm)
            st.append(f"s->cr = (s->cr & {u32(~mask)}) | ({G(rd)} & {u32(mask)});")
        elif m == "mcrf":
            st.append(f"cr_set_field(s, {i.crf_d}, cr_get_field(s, {i.crf_s}));")
        elif m == "mcrxr":
            st.append(f"cr_set_field(s, {i.crf_d}, s->xer >> 28); s->xer &= 0x0FFFFFFFu;")
        elif m in _CR_OPS:
            # XL-form: crbD, crbA, crbB sit in the rD, rA, rB slots.
            expr = _CR_OPS[m].format(a=f"cr_bit(s, {i.bi})", b=f"cr_bit(s, {rb})")
            st.append(f"cr_set_bit(s, {i.bo}, ({expr}) & 1);")
        elif m == "mfsr":
            st.append(f"{G(rd)} = s->sr[{ra & 0xF}];")
        elif m == "mtsr":
            st.append(f"s->sr[{ra & 0xF}] = {G(rd)};")
        elif m == "mfmsr":
            st.append(f"{G(rd)} = s->msr;")
        elif m == "mtmsr":
            st.append(f"s->msr = {G(rd)};")
        elif m in _NOP_MNEMONICS:
            pass

        # ---- branches -------------------------------------------------
        elif m == "b":
            if i.lk_bit and pc in self.savepoints:
                # A context-saving call: the thread parks here and is resumed
                # by a longjmp to this frame. See runtime/threads.c.
                st.append(
                    f"if (setjmp(*guest_savepoint(s)) == 0) {{ {self._call(i.target, nxt)} }} "
                    f"else {{ guest_resumed(s); }} /* {self.savepoints[pc]} */"
                )
            elif i.lk_bit:
                st.append(self._call(i.target, nxt))
            else:
                st.append(self._jump(i.target, addr_set, pc))
        elif m == "bc":
            if not (i.bo & 4):
                st.append("s->ctr--;")
            cond = self._cond(i.bo, i.bi)
            body = self._call(i.target, nxt) if i.lk_bit else self._jump(i.target, addr_set, pc)
            st.append(body if cond == "1" else f"if ({cond}) {{ {body} }}")
        elif m == "bclr":
            if not (i.bo & 4):
                st.append("s->ctr--;")
            cond = self._cond(i.bo, i.bi)
            if i.lk_bit:
                body = f"{{ uint32_t t = s->lr; s->lr = {u32(nxt)}; dispatch(s, t); }}"
            else:
                body = "return;"
            st.append(body if cond == "1" else f"if ({cond}) {body}")
        elif m == "bcctr":
            cond = self._cond(i.bo, i.bi)
            if i.lk_bit:
                body = f"s->lr = {u32(nxt)}; dispatch(s, s->ctr);"
                st.append(body if cond == "1" else f"if ({cond}) {{ {body} }}")
            elif pc in self.tables:
                st.append("switch (s->ctr) {")
                for t in sorted(set(self.tables[pc].targets)):
                    st.append(f"case {u32(t)}: {self._jump(t, addr_set, pc)}")
                st.append(f"default: guest_trap(s, {u32(pc)}); return;")
                st.append("}")
            else:
                self.stats.translated[m] -= 1
                return self._unsupported(i, "bctr-unresolved")
        elif m == "rfi":
            st.append("return;")
        elif m == "sc":
            st.append(f"guest_syscall(s, {u32(pc)});")
        elif m in ("tw", "twi"):
            st.append(f"guest_trap(s, {u32(pc)});")

        # ---- floating point -------------------------------------------
        elif m in _FLOAT_ARITH:
            expr, single = _FLOAT_ARITH[m]
            expr = expr.format(a=F(ra), b=F(rb), c=F(rc))
            if single:
                st.append(f"{F(rd)} = s->fpr[{rd}].ps1 = (double)(float)({expr});")
            else:
                st.append(f"{F(rd)} = {expr};")
            if i.rc_bit:
                st.append("cr_set_field(s, 1, (s->fpscr >> 28) & 0xFu);")
        elif m == "fmr":
            st.append(f"{F(rd)} = {F(rb)};")
        elif m == "fneg":
            st.append(f"{F(rd)} = -{F(rb)};")
        elif m == "fabs":
            st.append(f"{F(rd)} = fabs({F(rb)});")
        elif m == "fnabs":
            st.append(f"{F(rd)} = -fabs({F(rb)});")
        elif m == "frsp":
            st.append(f"{F(rd)} = (double)(float){F(rb)};")
        elif m == "fctiw":
            st.append(f"fpr_set_bits(s, {rd}, ppc_fctiw({F(rb)}, 0));")
        elif m == "fctiwz":
            st.append(f"fpr_set_bits(s, {rd}, ppc_fctiw({F(rb)}, 1));")
        elif m in ("fcmpu", "fcmpo"):
            st.append(f"cr_fcmp(s, {i.crf_d}, {F(ra)}, {F(rb)});")
        elif m == "mffs":
            st.append(f"fpr_set_bits(s, {rd}, (uint64_t)s->fpscr);")
        elif m == "mtfsf":
            mask = crm_mask(i.imm)
            st.append(
                f"s->fpscr = (s->fpscr & {u32(~mask)}) | ((uint32_t)fpr_bits(s, {rb}) & {u32(mask)});"
            )
        elif m in ("mtfsb0", "mtfsb1"):
            # The bit is crbD, bits 6-10: the rd field. This read `bo`, which
            # the X-form decode never sets, so every one touched FX (bit 0).
            bit = 31 - i.rd
            if m == "mtfsb0":
                st.append(f"s->fpscr &= ~(1u << {bit});")
            else:
                st.append(f"s->fpscr |= (1u << {bit});")

        # ---- paired singles (Gekko) -----------------------------------
        elif m in _PSQ:
            xform = m.endswith("x")
            ea = f"({GA(ra)} + {G(rb)})" if xform else f"({GA(ra)} + {u32(i.imm)})"
            helper = "psq_load" if m.startswith("psq_l") else "psq_store"
            body = f"{helper}(s, {rd}, {{ea}}, {i.gqr}, {i.quant_w});"
            if m in ("psq_lu", "psq_lux", "psq_stu", "psq_stux"):
                st.append(f"{{ uint32_t ea = {ea}; {body.format(ea='ea')} {G(ra)} = ea; }}")
            else:
                st.append(body.format(ea=ea))
        elif m in _PS_PAIR:
            e0, e1 = _PS_PAIR[m]
            kw = dict(a0=F(ra), a1=F1(ra), b0=F(rb), b1=F1(rb), c0=F(rc), c1=F1(rc))
            st.append(
                f"{{ double t0 = (double)(float)({e0.format(**kw)}); "
                f"double t1 = (double)(float)({e1.format(**kw)}); "
                f"{F(rd)} = t0; {F1(rd)} = t1; }}"
            )
            if i.rc_bit:
                st.append("cr_set_field(s, 1, (s->fpscr >> 28) & 0xFu);")
        elif m in ("ps_cmpu0", "ps_cmpo0"):
            st.append(f"cr_fcmp(s, {i.crf_d}, {F(ra)}, {F(rb)});")
        elif m in ("ps_cmpu1", "ps_cmpo1"):
            st.append(f"cr_fcmp(s, {i.crf_d}, {F1(ra)}, {F1(rb)});")

        else:
            self.stats.translated[m] -= 1
            return self._unsupported(i)

        return st
