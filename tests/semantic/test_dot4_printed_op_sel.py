"""A v_dot4_i32_i8 whose op_sel/op_sel_hi llvm-mc printed as the defaults takes the stored fields.

llvm-mc decodes a VOP3P v_dot4_i32_i8 with op_sel_hi other than [1,1,1] but prints and re-encodes it with op_sel 0 and
op_sel_hi 7, so the listing names halves the instruction does not read. translate() used to refuse it ("a field the
disassembler dropped"); it now reads the two fields from the blob when they are the only difference
(rdna2spirv.dot4_op_sel_fields), and the v_dot4_i32_i8 rule applies them as measured on gfx1036
(tests/semantic/test_alu_dot4_i8.py). Ported from luizwidmer/palmetto (GPL-3.0). Hand-built listing and blob, the
encodings assembled from the VOP3P field layout; no game data.

    python -I tests/semantic/test_dot4_printed_op_sel.py -v
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from support import mrt0_words, rdna2spirv, translate_listing  # noqa: E402

NAME = "ps_dot4_printed_op_sel"
CODE_START = 0x100
A, B, C = 0x80FF7F01, 0x12F0FE7F, 0x00001000


def dot4(op_sel=0, op_sel_hi=7, dst=0, srcs=(1, 2, 3)):
    """VOP3P v_dot4_i32_i8 (opcode 0x16) vdst, v[srcs] with the given op_sel and op_sel_hi."""
    w0 = 0xCC000000 | (0x16 << 16) | ((op_sel_hi >> 2) & 1) << 14 | (op_sel & 7) << 11 | dst
    w1 = (op_sel_hi & 3) << 27 | (256 + srcs[2]) << 18 | (256 + srcs[1]) << 9 | (256 + srcs[0])
    return struct.pack("<2I", w0, w1).hex(" ")


def v_mov(reg, value):
    return (f"v_mov_b32_e32 v{reg}, 0x{value:x}", f"ff {2 * reg:02x} 02 7e " + struct.pack("<I", value).hex(" "))


SETUP = [v_mov(1, A), v_mov(2, B), v_mov(3, C)]
TAIL = [("exp mrt0 v0, v0, v0, v0 done vm", "0f 18 00 f8 00 00 00 00"), ("s_endpgm", "00 00 81 bf")]
PRINTED = ("v_dot4_i32_i8 v0, v1, v2, v3", dot4())


def listing(program, end):
    lines = [f"; {NAME}  code 0x{CODE_START:x}..0x{end:x} (scan), gfx1030, 0 invalid words"]
    for text, enc in program:
        lines.append(f"\t{text:<40}; encoding: [{','.join(f'0x{b:02x}' for b in bytes.fromhex(enc))}]")
    return ("\n".join(lines) + "\n").encode("utf-8")


def ref(a, b, addend, op_sel=0, op_sel_hi=7):
    def byte(x, off):
        v = (x >> off) & 0xFF
        return v - 256 if v & 0x80 else v
    total = addend
    for k in range(4):
        half = (op_sel if k < 2 else op_sel_hi)
        xo = 16 * ((half >> 0) & 1) + 8 * (k & 1)
        yo = 16 * ((half >> 1) & 1) + 8 * (k & 1)
        total += byte(a, xo) * byte(b, yo)
    return total & 0xFFFFFFFF


def translate(stored_dot4, reanchor=False):
    printed = SETUP + [PRINTED] + TAIL
    stored = SETUP + [("", stored_dot4)] + TAIL
    code = b"".join(bytes.fromhex(enc) for _, enc in stored)
    data = listing(printed, CODE_START + len(code))
    blob = bytes(CODE_START) + code
    if reanchor:   # a listing with an undecodable word after the code goes through the re-anchoring path
        data += b"; warning: invalid instruction encoding\n"
        blob += b"\xff" * 4
    return translate_listing(NAME, data, fssm=blob)


class Dot4PrintedOpSelTest(unittest.TestCase):
    def test_reference_is_the_plain_dot_product_for_the_defaults(self):
        sb = [lambda x, k=k: (((x >> 8 * k) & 0xFF) ^ 0x80) - 0x80 for k in range(4)]
        self.assertEqual(ref(A, B, C), (sum(f(A) * f(B) for f in sb) + C) & 0xFFFFFFFF)

    def test_default_fields_translate_as_printed(self):
        self.assertEqual(mrt0_words(translate(dot4())), [ref(A, B, C)] * 4)

    def test_stored_op_sel_hi_is_read_from_the_blob(self):
        # op_sel_hi [1,0,1]: byte pairs 2..3 of S1 come from its low half. llvm-mc decodes it and prints [1,1,1].
        for reanchor in (False, True):
            with self.subTest(reanchor=reanchor):
                got = mrt0_words(translate(dot4(op_sel_hi=5), reanchor))
                self.assertEqual(got, [ref(A, B, C, op_sel_hi=5)] * 4)
                self.assertNotEqual(ref(A, B, C, op_sel_hi=5), ref(A, B, C))

    def test_stored_op_sel_on_source_0_is_read_from_the_blob(self):
        got = mrt0_words(translate(dot4(op_sel=1, op_sel_hi=6)))
        self.assertEqual(got, [ref(A, B, C, op_sel=1, op_sel_hi=6)] * 4)

    def test_a_non_default_s2_half_stays_refused(self):
        with self.assertRaises(rdna2spirv.Unsupported) as cm:
            translate(dot4(op_sel_hi=3))
        self.assertIn("op_sel", str(cm.exception))

    def test_another_difference_is_still_a_dropped_field(self):
        # The stored word names v4 as S2, the printed one v3: not an op_sel difference.
        with self.assertRaises(rdna2spirv.Unsupported) as cm:
            translate(dot4(op_sel_hi=5, srcs=(1, 2, 4)))
        self.assertIn("dropped", str(cm.exception))

    def test_fields_helper(self):
        printed = bytes.fromhex(dot4())
        self.assertEqual(rdna2spirv.dot4_op_sel_fields(bytes.fromhex(dot4(op_sel_hi=5)), printed,
                                                       "v_dot4_i32_i8"), (0, 5))
        self.assertIsNone(rdna2spirv.dot4_op_sel_fields(printed, printed, "v_dot4_i32_i8"))
        self.assertIsNone(rdna2spirv.dot4_op_sel_fields(bytes.fromhex(dot4(op_sel_hi=5)), printed,
                                                        "v_dot2_i32_i16"))


if __name__ == "__main__":
    unittest.main()
