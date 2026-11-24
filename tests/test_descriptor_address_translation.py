"""32-bit descriptor address translation contract (docs/d3d12_vulkan.md §3.3 and §3.4).

No game data, no GPU, no game executables. All descriptors and addresses are
hand-written to match the documented contract. Run with:

    python -m pytest tests/test_descriptor_address_translation.py

or:

    python -m pytest tests/test_descriptor_address_translation.py -v
"""
import struct

import pytest

# ---------------------------------------------------------------------------
# Contract constants (from docs/d3d12_vulkan.md §3.3)
# ---------------------------------------------------------------------------

FOUR_GIB = 1 << 32
ARENA_CAP_MAX = (4095 * 1024 * 1024)   # 4095 MiB — the documented arena cap
IMAGE_KEY_BASE = FOUR_GIB              # image keys start at exactly 4 GiB (§3.3)
TWO_POW_48 = 1 << 48
TWO_POW_40 = 1 << 40


# ---------------------------------------------------------------------------
# Pure-Python helpers mirroring the runtime's descriptor encoding
# ---------------------------------------------------------------------------

def encode_vsharp_base(addr48: int) -> tuple[int, int]:
    """Pack a 48-bit buffer address into V# word0 / word1 lower 16 bits.

    V# BASE_ADDRESS is 48 bits, split as:
        word0 = addr[31:0]
        word1[15:0] = addr[47:32]
    See gfx10_descriptors.h, tools/rdna2spirv.py BASE_ADDRESS comment.
    """
    assert 0 <= addr48 < TWO_POW_48, f"V# base must be < 2^48, got {addr48:#x}"
    word0 = addr48 & 0xFFFF_FFFF
    word1_lo16 = (addr48 >> 32) & 0xFFFF
    return word0, word1_lo16


def decode_vsharp_base(word0: int, word1: int) -> int:
    """Extract the 48-bit buffer base address from V# word0 and word1."""
    return word0 | ((word1 & 0xFFFF) << 32)


def encode_tsharp_base(addr: int) -> tuple[int, int]:
    """Pack a 256-byte-aligned image address into T# word0 / word1 lower 8 bits.

    T# BASE_ADDRESS is 40 bits, stored as address >> 8 split across:
        word0 = addr[39:8]   (i.e., addr >> 8, low 32 bits)
        word1[7:0] = addr[47:40] (high 8 bits of addr >> 8)
    gfx10_descriptors.h: "address >> 8 … 48 bits; word 0 … word 1[7:0]"
    """
    assert addr % 256 == 0, f"T# base must be 256-byte aligned, got {addr:#x}"
    assert 0 <= addr < TWO_POW_48, f"T# base must be < 2^48, got {addr:#x}"
    shifted = addr >> 8
    word0 = shifted & 0xFFFF_FFFF
    word1_lo8 = (shifted >> 32) & 0xFF
    return word0, word1_lo8


def decode_tsharp_base(word0: int, word1: int) -> int:
    """Extract the image base address from T# word0 and word1[7:0]."""
    shifted = word0 | ((word1 & 0xFF) << 32)
    return shifted << 8


# ---------------------------------------------------------------------------
# §3.3: Arena GPU address contract  host_address = heap_base + console_va
# ---------------------------------------------------------------------------

class TestArenaAddressContract:
    """Verify host_address = heap_base + console_va for arena-resident resources."""

    def test_buffer_address_round_trip_low_arena(self):
        """A buffer at a low arena console VA reconstructs correctly."""
        heap_base = 0x0000_0001_0000_0000  # an example BDA above 4 GiB for the arena buffer itself
        console_va = 0x0001_0000            # 64 KiB offset inside the arena (a 32-bit value)
        host_address = heap_base + console_va
        # Verify: strip heap_base → get back console_va
        recovered_va = (host_address - heap_base) & 0xFFFF_FFFF_FFFF_FFFF
        assert recovered_va == console_va

    def test_buffer_address_at_1gib_offset(self):
        heap_base = 0x0000_0002_0000_0000
        console_va = 0x4000_0000  # 1 GiB — still 32-bit
        assert console_va < FOUR_GIB
        host_address = heap_base + console_va
        recovered_va = (host_address - heap_base) & 0xFFFF_FFFF_FFFF_FFFF
        assert recovered_va == console_va

    def test_ptr32_truncation_gives_same_host_address(self):
        """25,865 shaders zero the high dword before the heap_base + va add.

        When console_va < 4 GiB, truncating to 32 bits and then adding heap_base
        must give the same result as adding the full 64-bit va.
        """
        heap_base = 0x0000_0001_0000_0000
        console_va = 0x0080_0000  # 8 MiB — fits in 32 bits
        va32 = console_va & 0xFFFF_FFFF    # ptr32_lo zero-extension
        # Both must land at the same host address:
        assert heap_base + console_va == heap_base + va32

    def test_ptr32_truncation_fails_for_high_arena_va(self):
        """A VA >= 4 GiB cannot survive ptr32 truncation — this is what forces
        shader-visible descriptor heaps to live below 4 GiB (§3.3 rationale).
        """
        heap_base = 0x0000_0001_0000_0000
        console_va_above_4gib = 0x1_0000_0000  # 4 GiB — does NOT fit in 32 bits
        va32 = console_va_above_4gib & 0xFFFF_FFFF  # truncated to 0
        # ptr32 path gives the wrong host address:
        assert heap_base + va32 != heap_base + console_va_above_4gib

    def test_all_descriptor_heap_vas_must_be_below_4gib(self):
        """Descriptor heaps are allocated in the low 4 GiB of the arena."""
        arena_cap = ARENA_CAP_MAX
        # Every arena offset within the cap must be < 4 GiB for ptr32_lo safety:
        assert arena_cap < FOUR_GIB, (
            f"Arena cap {arena_cap:#x} must be < 4 GiB ({FOUR_GIB:#x})"
        )

    def test_image_key_range_starts_above_arena(self):
        """Image keys start at 4 GiB, above the 4095 MiB arena cap (§3.3)."""
        assert IMAGE_KEY_BASE > ARENA_CAP_MAX
        assert IMAGE_KEY_BASE == FOUR_GIB

    @pytest.mark.parametrize("console_va", [
        0x0001_0000,     # 64 KiB
        0x0010_0000,     # 1 MiB
        0x0100_0000,     # 16 MiB
        0x1000_0000,     # 256 MiB
        0xF000_0000,     # ~3.75 GiB — still below 4 GiB
    ])
    def test_heap_base_plus_console_va_parametric(self, console_va):
        heap_base = 0x0000_0001_8000_0000  # an arbitrary BDA > 4 GiB
        host_address = heap_base + console_va
        # Round-trip: subtract heap_base to recover console_va
        assert (host_address - heap_base) == console_va
        # ptr32_lo safety: console_va below 4 GiB so truncation is lossless
        assert (console_va & 0xFFFF_FFFF) == console_va


# ---------------------------------------------------------------------------
# §3.4: V# (buffer descriptor) base address encoding
# ---------------------------------------------------------------------------

class TestVSharpBaseAddress:
    """Verify the 48-bit V# base address encode/decode contract.

    tools/rdna2spirv.py reads: BASE_ADDRESS = word0 | (word1 & 0xFFFF) << 32
    gfx10_descriptors.h writes the same split.
    """

    def test_zero_base(self):
        w0, w1_lo = encode_vsharp_base(0)
        assert w0 == 0
        assert w1_lo == 0
        assert decode_vsharp_base(w0, w1_lo) == 0

    def test_low_32bit_base(self):
        addr = 0x0010_0000  # 1 MiB — fits in word0 alone
        w0, w1_lo = encode_vsharp_base(addr)
        assert w0 == addr
        assert w1_lo == 0
        assert decode_vsharp_base(w0, w1_lo) == addr

    def test_high_16bit_spills_into_word1(self):
        # Place a bit in bit 32 so it goes into word1[0]
        addr = 0x0001_0000_0000  # 4 GiB
        w0, w1_lo = encode_vsharp_base(addr)
        assert w0 == 0
        assert w1_lo == 0x0001
        assert decode_vsharp_base(w0, w1_lo) == addr

    def test_max_representable_48bit_address(self):
        addr = TWO_POW_48 - 1  # 0xFFFF_FFFF_FFFF
        w0, w1_lo = encode_vsharp_base(addr)
        assert w0 == 0xFFFF_FFFF
        assert w1_lo == 0xFFFF
        assert decode_vsharp_base(w0, w1_lo) == addr

    def test_typical_arena_buffer_address(self):
        """A realistic arena buffer VA like 0x40000 (256 KiB) encodes cleanly."""
        addr = 0x0004_0000
        w0, w1_lo = encode_vsharp_base(addr)
        assert decode_vsharp_base(w0, w1_lo) == addr

    def test_word1_upper_16bits_are_not_base(self):
        """Only word1[15:0] carries the base; upper bits belong to other fields."""
        # Simulate a full word1 with other fields in upper 16 bits
        addr = 0x0001_0000_0000  # 4 GiB; word1_lo = 0x0001
        w0, w1_lo = encode_vsharp_base(addr)
        # Imagine word1 has stride in bits [29:16] and something in [31:30]:
        word1_full = w1_lo | (0x20 << 16)  # stride=32 in bits[29:16], example
        # decode must only look at [15:0]:
        assert decode_vsharp_base(w0, word1_full) == addr

    @pytest.mark.parametrize("addr", [
        0x0000_0000,
        0x0001_0000,
        0x0040_0000,
        0x0FFF_0000,          # near 256 MiB arena default
        0x0001_0000_0000,     # 4 GiB
        0x000F_FFFF_FF00,     # arbitrary high 48-bit value
    ])
    def test_round_trip_parametric(self, addr):
        w0, w1_lo = encode_vsharp_base(addr)
        assert decode_vsharp_base(w0, w1_lo) == addr


# ---------------------------------------------------------------------------
# §3.4: T# (image descriptor) base address encoding
# ---------------------------------------------------------------------------

class TestTSharpBaseAddress:
    """Verify the 48-bit T# BASE_ADDRESS encoding.

    RDNA2 ISA §8.2.6, Table 45: T# BASE_ADDRESS = image_address >> 8 (40 bits).
    gfx10_descriptors.h: word0 = addr >> 8 (low 32 bits), word1[7:0] = addr >> 40.
    """

    def test_zero_base(self):
        w0, w1_lo8 = encode_tsharp_base(0)
        assert w0 == 0 and w1_lo8 == 0
        assert decode_tsharp_base(w0, w1_lo8) == 0

    def test_minimum_nonzero_address(self):
        """Smallest nonzero T# address is 256 (one unit of 256-byte alignment)."""
        addr = 256
        w0, w1_lo8 = encode_tsharp_base(addr)
        assert w0 == 1
        assert w1_lo8 == 0
        assert decode_tsharp_base(w0, w1_lo8) == addr

    def test_misaligned_raises(self):
        with pytest.raises(AssertionError):
            encode_tsharp_base(0x100 + 1)  # not 256-byte aligned

    def test_image_key_base_at_4gib(self):
        """Image keys start at 4 GiB (0x1_0000_0000) which is 256-byte aligned."""
        addr = IMAGE_KEY_BASE  # 0x1_0000_0000
        w0, w1_lo8 = encode_tsharp_base(addr)
        assert decode_tsharp_base(w0, w1_lo8) == addr

    def test_typical_key_just_above_4gib(self):
        """A second image key at 4 GiB + 256 bytes."""
        addr = IMAGE_KEY_BASE + 256
        w0, w1_lo8 = encode_tsharp_base(addr)
        assert decode_tsharp_base(w0, w1_lo8) == addr

    def test_max_representable_tsharp_address(self):
        """Maximum T# address: 0xFFFFFFFFFF00 (2^48 − 256)."""
        addr = 0xFFFF_FFFF_FF00
        w0, w1_lo8 = encode_tsharp_base(addr)
        assert decode_tsharp_base(w0, w1_lo8) == addr

    def test_word1_upper_bits_not_read(self):
        """T# word1 carries other fields (MIN_LOD, FORMAT, WIDTH_LO) above bit 7.
        decode must only look at word1[7:0].
        """
        addr = IMAGE_KEY_BASE  # 4 GiB
        w0, w1_lo8 = encode_tsharp_base(addr)
        # Simulate FORMAT in bits [28:20] and WIDTH_LO in [31:30]:
        word1_full = w1_lo8 | (0x14 << 20) | (0x1 << 30)
        assert decode_tsharp_base(w0, word1_full) == addr

    @pytest.mark.parametrize("addr", [
        0,
        256,
        0x1000,
        0x0010_0000,          # 1 MiB
        0x1_0000_0000,        # 4 GiB (image key base)
        0x1_0000_0100,        # 4 GiB + 256
        0xFFFF_FFFF_FF00,     # maximum representable
    ])
    def test_round_trip_parametric(self, addr):
        w0, w1_lo8 = encode_tsharp_base(addr)
        assert decode_tsharp_base(w0, w1_lo8) == addr


# ---------------------------------------------------------------------------
# §3.3 Descriptor-table high-dword truncation census (ptr32_lo contract)
# ---------------------------------------------------------------------------

class TestPtr32LoContract:
    """Model the 25,865-shader census finding: descriptor-table pointers are ptr32_lo.

    A ptr32_lo read means the shader uses only the low 32 bits of a user SGPR
    (the high dword is forced to 0 by the shader compiler). This is correct iff
    all arena VAs fit in 32 bits — i.e., the arena cap is < 4 GiB.
    """

    # Synthetic 'corpus' of (console_va, expected_host_address) pairs using
    # ptr32_lo arithmetic: host = heap_base + (va & 0xFFFF_FFFF)
    HEAP_BASE = 0x0000_0002_0000_0000  # a hypothetical BDA for the arena VkBuffer

    def _ptr32_host(self, va: int) -> int:
        return self.HEAP_BASE + (va & 0xFFFF_FFFF)

    @pytest.mark.parametrize("va,label", [
        (0x0001_0000, "64 KiB"),
        (0x0010_0000, "1 MiB"),
        (0x0100_0000, "16 MiB"),
        (0x1000_0000, "256 MiB"),
        (0x3FFF_0000, "near 1 GiB"),
        (0xEFFF_0000, "near 3.75 GiB — highest valid arena VA"),
    ])
    def test_ptr32_host_address_is_correct_for_low_arena_vas(self, va, label):
        """VA fits in 32 bits → ptr32_lo truncation is lossless."""
        assert va < FOUR_GIB, f"{label}: VA must be < 4 GiB for ptr32_lo safety"
        full_host = self.HEAP_BASE + va
        ptr32_host = self._ptr32_host(va)
        assert full_host == ptr32_host, (
            f"{label}: ptr32_lo gives wrong address "
            f"({ptr32_host:#x} != {full_host:#x})"
        )

    def test_ptr32_breaks_for_high_va(self):
        """VA >= 4 GiB truncated to 32 bits gives the wrong host address.
        This is why the arena cap is 4095 MiB (< 4 GiB).
        """
        va_above_4gib = 0x1_8000_0000  # 6 GiB — above arena cap
        full_host = self.HEAP_BASE + va_above_4gib
        ptr32_host = self._ptr32_host(va_above_4gib)  # loses the high bit
        assert full_host != ptr32_host, (
            "ptr32_lo should corrupt addresses >= 4 GiB"
        )

    def test_arena_cap_below_4gib_satisfies_ptr32_contract(self):
        """The 4095 MiB arena cap keeps every console VA within 32 bits."""
        assert ARENA_CAP_MAX < FOUR_GIB
        # Every allocation at or below the cap has a 32-bit VA:
        for offset in [0, 0x1000, 0x0010_0000, ARENA_CAP_MAX]:
            assert (offset & 0xFFFF_FFFF) == offset, (
                f"Arena offset {offset:#x} does not fit in 32 bits — "
                "violates ptr32_lo contract"
            )


# ---------------------------------------------------------------------------
# §3.3: BVH builder 40-bit address constraint
# ---------------------------------------------------------------------------

class TestBvhBuilderAddressConstraint:
    """The BVH builder rejects destination addresses >= 2^40 (measured, §3.3).

    Arena VAs are at most 4095 MiB (< 2^32) so they always pass. Biased
    upload-heap VAs can be >= 2^40 and would fail.
    """

    def _bvh_validate(self, addr: int) -> bool:
        """Mirror of the BVH builder's check: addr < 2^40 AND 256-byte aligned."""
        return (addr < TWO_POW_40) and (addr % 256 == 0)

    @pytest.mark.parametrize("va", [0, 256, 0x1000, 0xFFFF_FF00, ARENA_CAP_MAX & ~0xFF])
    def test_arena_vas_pass_bvh_check(self, va):
        """All arena VAs (< 4095 MiB, 256-byte aligned) pass the BVH check."""
        aligned = va & ~0xFF  # round down to 256-byte boundary
        assert self._bvh_validate(aligned), (
            f"Arena VA {aligned:#x} should pass BVH builder check"
        )

    @pytest.mark.parametrize("bad_va", [
        0x01_0000_0000_0000,   # 2^48 — way above 2^40
        TWO_POW_40,            # exact boundary — rejects with jae
        TWO_POW_40 + 256,      # just above
    ])
    def test_high_vas_fail_bvh_check(self, bad_va):
        """Addresses >= 2^40 fail the BVH builder's validation."""
        # Align down to satisfy the alignment requirement so only the range check matters:
        aligned = bad_va & ~0xFF
        # Note: if aligned is still >= 2^40, the range check fires:
        if aligned >= TWO_POW_40:
            assert not self._bvh_validate(aligned), (
                f"VA {aligned:#x} >= 2^40 should fail BVH builder check"
            )
