// Console DirectStorage shims for XCRDAPI.dll, dstorage_xs.dll, and dstorage.dll.
//
// Serves DStorageGetFactory and minimal synchronous-I/O COM implementations of IDStorageFactoryX,
// IDStorageFileX, IDStorageQueueX, and IDStorageStatusArrayX tailored to the Xbox Series console ABI
// (notably Destination preceding Source in DSTORAGE_REQUEST, and IDStorageQueueX without EnqueueSetEvent so
// Submit sits at slot 6 and Close at slot 8).
//
// File reads resolve paths through rt::fs::open_read_only (GAME:, G:, T:, overlay precedence, deletion markers,
// and traversal checks). Memory-source transfers copy directly; uncompressed transfers and RFC 1950/1951 zlib
// streams (low compression byte 0 and 1) are supported cleanly with zero _Init_thread_header static guards.
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "file_system.h"
#include "shims.h"

namespace rt::dstorage {
namespace {

struct SrwLockGuard {
    SRWLOCK* lock;
    explicit SrwLockGuard(SRWLOCK& l) : lock(&l) { ::AcquireSRWLockExclusive(lock); }
    ~SrwLockGuard() { ::ReleaseSRWLockExclusive(lock); }
    SrwLockGuard(const SrwLockGuard&) = delete;
    SrwLockGuard& operator=(const SrwLockGuard&) = delete;
};

// Public COM IIDs (IUnknown, Xbox Series DirectStorage X interfaces, and PC DirectStorage equivalents).
constexpr GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidFactoryX = {0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr GUID kIidFactoryPc = {0x6924ea0c, 0xc3cd, 0x4826, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};
constexpr GUID kIidFileX = {0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
constexpr GUID kIidFilePc = {0x5de95e7b, 0x955a, 0x4868, {0xa7, 0x3c, 0x24, 0x3b, 0x29, 0xf4, 0xb8, 0xda}};
constexpr GUID kIidStatusArrayX = {0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr GUID kIidStatusArrayPc = {0x82397587, 0x7cd5, 0x453b, {0xa0, 0x2e, 0x31, 0x37, 0x9b, 0xd6, 0x46, 0x56}};
constexpr GUID kIidQueueX = {0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
constexpr GUID kIidQueuePc = {0xcfdbd83f, 0x9e06, 0x4fda, {0x8e, 0xa5, 0x69, 0x04, 0x21, 0x37, 0xf4, 0x9b}};

bool guid_equal(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

// Console queue descriptor layout (32 bytes on x64).
struct QueueDescX {
    uint64_t SourceType;
    uint16_t Capacity;
    uint8_t Priority;
    uint8_t Reserved[5];
    const char* Name;
    void* Device;
};
static_assert(sizeof(QueueDescX) == 32, "DSTORAGE_QUEUE_DESC console size mismatch");
static_assert(offsetof(QueueDescX, Capacity) == 0x08, "Capacity offset mismatch");
static_assert(offsetof(QueueDescX, Priority) == 0x0a, "Priority offset mismatch");
static_assert(offsetof(QueueDescX, Name) == 0x10, "Name offset mismatch");
static_assert(offsetof(QueueDescX, Device) == 0x18, "Device offset mismatch");

// Console request layout: Destination precedes Source; bit 22 selects memory source, low byte selects codec.
struct RequestOptionsX {
    uint64_t raw;
    uint8_t compression() const { return static_cast<uint8_t>(raw & 0xFFu); }
    bool source_is_memory() const { return ((raw >> 22) & 1u) != 0; }
};

struct RequestX {
    RequestOptionsX Options;
    void* DestinationPtr;
    uint64_t DestinationExtra;
    uint32_t DestinationSize;
    uint32_t Reserved0;
    const void* SourcePtr;
    uint64_t SourceOffset;
    uint32_t SourceSize;
    uint32_t Reserved1;
    const char* Name;
};
static_assert(offsetof(RequestX, DestinationPtr) == 0x08, "DestinationPtr offset mismatch");
static_assert(offsetof(RequestX, DestinationSize) == 0x18, "DestinationSize offset mismatch");
static_assert(offsetof(RequestX, SourcePtr) == 0x20, "SourcePtr offset mismatch");
static_assert(offsetof(RequestX, SourceOffset) == 0x28, "SourceOffset offset mismatch");
static_assert(offsetof(RequestX, SourceSize) == 0x30, "SourceSize offset mismatch");

// ---- Minimal RFC 1950/1951 zlib inflate (zero _Init_thread_header statics) ------------------------------------

struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool err = false;

    uint32_t read_bits(int n) {
        while (bitcnt < n) {
            if (pos >= size) {
                err = true;
                return 0;
            }
            bitbuf |= static_cast<uint32_t>(data[pos++]) << bitcnt;
            bitcnt += 8;
        }
        const uint32_t out = bitbuf & ((1u << n) - 1u);
        bitbuf >>= n;
        bitcnt -= n;
        return out;
    }

    void align_byte() {
        const int drop = bitcnt & 7;
        bitbuf >>= drop;
        bitcnt -= drop;
    }

    bool read_byte(uint8_t& out) {
        if (bitcnt >= 8) {
            out = static_cast<uint8_t>(bitbuf & 0xFFu);
            bitbuf >>= 8;
            bitcnt -= 8;
            return true;
        }
        if (pos >= size) {
            err = true;
            return false;
        }
        out = data[pos++];
        return true;
    }
};

constexpr int kMaxBits = 15;
constexpr int kMaxSymbols = 288;

struct HuffmanTable {
    int counts[kMaxBits + 1]{};
    int symbols[kMaxSymbols]{};
};

bool build_huffman(HuffmanTable& tbl, const int* lengths, int count) {
    for (int i = 0; i <= kMaxBits; ++i) tbl.counts[i] = 0;
    for (int i = 0; i < count; ++i) {
        if (lengths[i] < 0 || lengths[i] > kMaxBits) return false;
        tbl.counts[lengths[i]]++;
    }
    if (tbl.counts[0] == count) return true;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= tbl.counts[len];
        if (left < 0) return false;
    }
    int offsets[kMaxBits + 1]{};
    for (int len = 1; len < kMaxBits; ++len) offsets[len + 1] = offsets[len] + tbl.counts[len];
    for (int sym = 0; sym < count; ++sym) {
        if (lengths[sym] != 0) tbl.symbols[offsets[lengths[sym]]++] = sym;
    }
    return true;
}

int decode_huffman(BitReader& r, const HuffmanTable& tbl) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        code |= static_cast<int>(r.read_bits(1));
        if (r.err) return -1;
        const int count = tbl.counts[len];
        if (code - count < first) return tbl.symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

bool decode_block_codes(BitReader& r, std::vector<uint8_t>& out, size_t max_out, const HuffmanTable& lit,
                        const HuffmanTable& dist) {
    constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        const int sym = decode_huffman(r, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= max_out) return false;
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            if (sym > 285) return false;
            const int lidx = sym - 257;
            const size_t length = static_cast<size_t>(kLenBase[lidx]) + r.read_bits(kLenExtra[lidx]);
            const int d = decode_huffman(r, dist);
            if (d < 0 || d >= 30 || r.err) return false;
            const size_t distance = static_cast<size_t>(kDistBase[d]) + r.read_bits(kDistExtra[d]);
            if (r.err || distance == 0 || distance > out.size() || out.size() + length > max_out) return false;
            const size_t from = out.size() - distance;
            for (size_t i = 0; i < length; ++i) out.push_back(out[from + i]);
        }
    }
}

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t s1 = 1;
    uint32_t s2 = 0;
    for (size_t i = 0; i < len; ++i) {
        s1 = (s1 + data[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16) | s1;
}

bool inflate_zlib(const uint8_t* in, size_t in_size, std::vector<uint8_t>& out, size_t expected_size) {
    out.clear();
    if (!in || in_size < 6) return false;
    const uint8_t cmf = in[0];
    const uint8_t flg = in[1];
    if ((cmf & 0x0Fu) != 8 || (cmf >> 4) > 7) return false;
    if ((((static_cast<uint32_t>(cmf) << 8) | flg) % 31u) != 0 || (flg & 0x20u) != 0) return false;

    out.reserve(expected_size);
    BitReader r{in, in_size, 2, 0, 0, false};

    for (bool last = false; !last;) {
        last = r.read_bits(1) != 0;
        const uint32_t btype = r.read_bits(2);
        if (r.err) return false;
        if (btype == 0) {
            r.align_byte();
            uint8_t hdr[4]{};
            for (auto& b : hdr) {
                if (!r.read_byte(b)) return false;
            }
            const uint32_t len = static_cast<uint32_t>(hdr[0]) | (static_cast<uint32_t>(hdr[1]) << 8);
            const uint32_t nlen = static_cast<uint32_t>(hdr[2]) | (static_cast<uint32_t>(hdr[3]) << 8);
            if (len != (~nlen & 0xFFFFu) || out.size() + len > expected_size) return false;
            for (uint32_t i = 0; i < len; ++i) {
                uint8_t b = 0;
                if (!r.read_byte(b)) return false;
                out.push_back(b);
            }
        } else if (btype == 1) {
            int lit_len[288];
            int dist_len[32];
            for (int i = 0; i <= 143; ++i) lit_len[i] = 8;
            for (int i = 144; i <= 255; ++i) lit_len[i] = 9;
            for (int i = 256; i <= 279; ++i) lit_len[i] = 7;
            for (int i = 280; i <= 287; ++i) lit_len[i] = 8;
            for (int i = 0; i < 32; ++i) dist_len[i] = 5;
            HuffmanTable lit{}, dist{};
            if (!build_huffman(lit, lit_len, 288) || !build_huffman(dist, dist_len, 32)) return false;
            if (!decode_block_codes(r, out, expected_size, lit, dist)) return false;
        } else if (btype == 2) {
            const int hlit = static_cast<int>(r.read_bits(5)) + 257;
            const int hdist = static_cast<int>(r.read_bits(5)) + 1;
            const int hclen = static_cast<int>(r.read_bits(4)) + 4;
            if (r.err || hlit > 286 || hdist > 30) return false;
            constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            int cl_len[19]{};
            for (int i = 0; i < hclen; ++i) cl_len[kOrder[i]] = static_cast<int>(r.read_bits(3));
            if (r.err) return false;
            HuffmanTable cl{};
            if (!build_huffman(cl, cl_len, 19)) return false;
            int lengths[286 + 30]{};
            int n = 0;
            while (n < hlit + hdist) {
                const int sym = decode_huffman(r, cl);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[n++] = sym;
                } else {
                    int rep = 0;
                    int val = 0;
                    if (sym == 16) {
                        if (n == 0) return false;
                        val = lengths[n - 1];
                        rep = 3 + static_cast<int>(r.read_bits(2));
                    } else if (sym == 17) {
                        rep = 3 + static_cast<int>(r.read_bits(3));
                    } else {
                        rep = 11 + static_cast<int>(r.read_bits(7));
                    }
                    if (r.err || n + rep > hlit + hdist) return false;
                    for (int i = 0; i < rep; ++i) lengths[n++] = val;
                }
            }
            HuffmanTable lit{}, dist{};
            if (!build_huffman(lit, lengths, hlit) || !build_huffman(dist, lengths + hlit, hdist)) return false;
            if (!decode_block_codes(r, out, expected_size, lit, dist)) return false;
        } else {
            return false;
        }
    }

    r.align_byte();
    uint8_t sum_bytes[4]{};
    for (auto& b : sum_bytes) {
        if (!r.read_byte(b)) return false;
    }
    const uint32_t expected_adler = (static_cast<uint32_t>(sum_bytes[0]) << 24) |
                                    (static_cast<uint32_t>(sum_bytes[1]) << 16) |
                                    (static_cast<uint32_t>(sum_bytes[2]) << 8) | static_cast<uint32_t>(sum_bytes[3]);
    return adler32(out.data(), out.size()) == expected_adler;
}

// ---- IDStorageFileX ------------------------------------------------------------------------------------------

struct FileObject;
struct FileVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(FileObject* self, const IID* riid, void** ppv);
    ULONG(STDMETHODCALLTYPE* AddRef)(FileObject* self);
    ULONG(STDMETHODCALLTYPE* Release)(FileObject* self);
    void(STDMETHODCALLTYPE* Close)(FileObject* self);
    HRESULT(STDMETHODCALLTYPE* GetFileInformation)(FileObject* self, BY_HANDLE_FILE_INFORMATION* info);
};

struct FileObject {
    const FileVtbl* vtbl = nullptr;
    std::atomic<ULONG> refs{1};
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE handle = INVALID_HANDLE_VALUE;
    uint64_t size = 0;

    HRESULT read_at(uint64_t offset, void* dst, uint32_t bytes) {
        SrwLockGuard guard(lock);
        if (handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        if (offset > size || static_cast<uint64_t>(bytes) > size - offset) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);

        uint8_t* out = static_cast<uint8_t*>(dst);
        uint32_t remaining = bytes;
        uint64_t cur = offset;
        while (remaining > 0) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(cur & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(cur >> 32);
            DWORD got = 0;
            if (!::ReadFile(handle, out, remaining, &got, &ov)) {
                const DWORD err = ::GetLastError();
                return HRESULT_FROM_WIN32(err ? err : ERROR_READ_FAULT);
            }
            if (got == 0) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
            out += got;
            cur += got;
            remaining -= got;
        }
        return S_OK;
    }
};

HRESULT STDMETHODCALLTYPE file_qi(FileObject* self, const IID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!riid) return E_INVALIDARG;
    if (guid_equal(*riid, kIidUnknown) || guid_equal(*riid, kIidFileX) || guid_equal(*riid, kIidFilePc)) {
        self->vtbl->AddRef(self);
        *ppv = self;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE file_addref(FileObject* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

void STDMETHODCALLTYPE file_close(FileObject* self) {
    SrwLockGuard guard(self->lock);
    if (self->handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(self->handle);
        self->handle = INVALID_HANDLE_VALUE;
    }
}

ULONG STDMETHODCALLTYPE file_release(FileObject* self) {
    const ULONG rem = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        file_close(self);
        delete self;
    }
    return rem;
}

HRESULT STDMETHODCALLTYPE file_get_info(FileObject* self, BY_HANDLE_FILE_INFORMATION* info) {
    if (!info) return E_POINTER;
    SrwLockGuard guard(self->lock);
    if (self->handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
    if (!::GetFileInformationByHandle(self->handle, info)) {
        const DWORD err = ::GetLastError();
        return HRESULT_FROM_WIN32(err ? err : ERROR_GEN_FAILURE);
    }
    return S_OK;
}

const FileVtbl kFileVtbl = {file_qi, file_addref, file_release, file_close, file_get_info};

// ---- IDStorageStatusArrayX -----------------------------------------------------------------------------------

struct StatusArrayObject;
struct StatusArrayVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(StatusArrayObject* self, const IID* riid, void** ppv);
    ULONG(STDMETHODCALLTYPE* AddRef)(StatusArrayObject* self);
    ULONG(STDMETHODCALLTYPE* Release)(StatusArrayObject* self);
    bool(STDMETHODCALLTYPE* IsComplete)(StatusArrayObject* self, UINT32 index);
    HRESULT(STDMETHODCALLTYPE* GetHResult)(StatusArrayObject* self, UINT32 index);
};

struct StatusSlot {
    bool complete = true;
    HRESULT result = S_OK;
};

struct StatusArrayObject {
    const StatusArrayVtbl* vtbl = nullptr;
    std::atomic<ULONG> refs{1};
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<StatusSlot> slots;
};

HRESULT STDMETHODCALLTYPE status_qi(StatusArrayObject* self, const IID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!riid) return E_INVALIDARG;
    if (guid_equal(*riid, kIidUnknown) || guid_equal(*riid, kIidStatusArrayX) || guid_equal(*riid, kIidStatusArrayPc)) {
        self->vtbl->AddRef(self);
        *ppv = self;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE status_addref(StatusArrayObject* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE status_release(StatusArrayObject* self) {
    const ULONG rem = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}

bool STDMETHODCALLTYPE status_is_complete(StatusArrayObject* self, UINT32 index) {
    SrwLockGuard guard(self->lock);
    if (index >= self->slots.size()) return false;
    return self->slots[index].complete;
}

HRESULT STDMETHODCALLTYPE status_get_hresult(StatusArrayObject* self, UINT32 index) {
    SrwLockGuard guard(self->lock);
    if (index >= self->slots.size()) return E_INVALIDARG;
    if (!self->slots[index].complete) return E_PENDING;
    return self->slots[index].result;
}

const StatusArrayVtbl kStatusArrayVtbl = {status_qi, status_addref, status_release, status_is_complete,
                                          status_get_hresult};

// ---- IDStorageQueueX -----------------------------------------------------------------------------------------

struct QueueObject;
struct QueueVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(QueueObject* self, const IID* riid, void** ppv);
    ULONG(STDMETHODCALLTYPE* AddRef)(QueueObject* self);
    ULONG(STDMETHODCALLTYPE* Release)(QueueObject* self);
    void(STDMETHODCALLTYPE* EnqueueRequest)(QueueObject* self, const RequestX* request);
    void(STDMETHODCALLTYPE* EnqueueStatus)(QueueObject* self, StatusArrayObject* status_array, UINT32 index);
    void(STDMETHODCALLTYPE* EnqueueSignal)(QueueObject* self, void* fence, UINT64 value);
    void(STDMETHODCALLTYPE* Submit)(QueueObject* self);
    void(STDMETHODCALLTYPE* CancelRequestsWithTag)(QueueObject* self, UINT64 mask, UINT64 value);
    void(STDMETHODCALLTYPE* Close)(QueueObject* self);
    HANDLE(STDMETHODCALLTYPE* GetErrorEvent)(QueueObject* self);
    void(STDMETHODCALLTYPE* RetrieveErrorRecord)(QueueObject* self, void* record, UINT32 size);
    void(STDMETHODCALLTYPE* Query)(QueueObject* self, void* info);
};

struct QueuedItem {
    enum class Kind { Request, Status };
    Kind kind = Kind::Request;
    RequestX request{};
    FileObject* file = nullptr;
    StatusArrayObject* status_array = nullptr;
    UINT32 status_index = 0;
};

struct QueueObject {
    const QueueVtbl* vtbl = nullptr;
    std::atomic<ULONG> refs{1};
    SRWLOCK lock = SRWLOCK_INIT;
    bool closed = false;
    uint16_t capacity = 0;
    std::vector<QueuedItem> pending;
};

HRESULT execute_request(const RequestX& req, FileObject* file) {
    if (req.DestinationSize == 0 && req.SourceSize == 0) return S_OK;
    if (!req.DestinationPtr || req.DestinationSize == 0) return E_INVALIDARG;

    const uint8_t codec = req.Options.compression();
    if (codec != 0 && codec != 1) {
        std::fprintf(stderr, "dstorage: unsupported compression codec 0x%02x\n", codec);
        return E_NOTIMPL;
    }

    const bool from_mem = req.Options.source_is_memory();
    if (from_mem) {
        if (!req.SourcePtr) return E_INVALIDARG;
    } else if (!file) {
        return E_INVALIDARG;
    }

    if (codec == 0) {
        if (req.SourceSize > req.DestinationSize) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        if (from_mem) {
            const uint8_t* src = static_cast<const uint8_t*>(req.SourcePtr) + req.SourceOffset;
            std::memcpy(req.DestinationPtr, src, req.SourceSize);
            return S_OK;
        }
        return file->read_at(req.SourceOffset, req.DestinationPtr, req.SourceSize);
    }

    // Zlib-compressed source payload -> temporary buffer -> RFC 1950 inflate into DestinationPtr.
    std::vector<uint8_t> compressed(req.SourceSize);
    if (req.SourceSize > 0) {
        if (from_mem) {
            const uint8_t* src = static_cast<const uint8_t*>(req.SourcePtr) + req.SourceOffset;
            std::memcpy(compressed.data(), src, req.SourceSize);
        } else {
            const HRESULT hr = file->read_at(req.SourceOffset, compressed.data(), req.SourceSize);
            if (FAILED(hr)) return hr;
        }
    }

    std::vector<uint8_t> inflated;
    if (!inflate_zlib(compressed.data(), compressed.size(), inflated, req.DestinationSize) ||
        inflated.size() != req.DestinationSize) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    std::memcpy(req.DestinationPtr, inflated.data(), inflated.size());
    return S_OK;
}

void release_item_refs(QueuedItem& item) {
    if (item.file) {
        item.file->vtbl->Release(item.file);
        item.file = nullptr;
    }
    if (item.status_array) {
        item.status_array->vtbl->Release(item.status_array);
        item.status_array = nullptr;
    }
}

HRESULT STDMETHODCALLTYPE queue_qi(QueueObject* self, const IID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!riid) return E_INVALIDARG;
    if (guid_equal(*riid, kIidUnknown) || guid_equal(*riid, kIidQueueX) || guid_equal(*riid, kIidQueuePc)) {
        self->vtbl->AddRef(self);
        *ppv = self;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE queue_addref(QueueObject* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

void STDMETHODCALLTYPE queue_close(QueueObject* self) {
    std::vector<QueuedItem> dropped;
    {
        SrwLockGuard guard(self->lock);
        self->closed = true;
        dropped.swap(self->pending);
    }
    for (auto& item : dropped) release_item_refs(item);
}

ULONG STDMETHODCALLTYPE queue_release(QueueObject* self) {
    const ULONG rem = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        queue_close(self);
        delete self;
    }
    return rem;
}

void STDMETHODCALLTYPE queue_enqueue_request(QueueObject* self, const RequestX* request) {
    if (!request) return;
    QueuedItem item;
    item.kind = QueuedItem::Kind::Request;
    item.request = *request;
    if (!request->Options.source_is_memory() && request->SourcePtr) {
        // Store FileObject pointer with an owned reference while queued; uncasted from const void*.
        item.file = reinterpret_cast<FileObject*>(const_cast<void*>(request->SourcePtr));
        item.file->vtbl->AddRef(item.file);
    }

    SrwLockGuard guard(self->lock);
    if (self->closed) {
        release_item_refs(item);
        return;
    }
    self->pending.push_back(item);
}

void STDMETHODCALLTYPE queue_enqueue_status(QueueObject* self, StatusArrayObject* status_array, UINT32 index) {
    if (!status_array) return;
    {
        SrwLockGuard sguard(status_array->lock);
        if (index < status_array->slots.size()) {
            status_array->slots[index].complete = false;
            status_array->slots[index].result = E_PENDING;
        }
    }

    QueuedItem item;
    item.kind = QueuedItem::Kind::Status;
    item.status_array = status_array;
    item.status_array->vtbl->AddRef(item.status_array);
    item.status_index = index;

    SrwLockGuard guard(self->lock);
    if (self->closed) {
        release_item_refs(item);
        return;
    }
    self->pending.push_back(item);
}

void STDMETHODCALLTYPE queue_enqueue_signal(QueueObject*, void*, UINT64) {}

void STDMETHODCALLTYPE queue_submit(QueueObject* self) {
    std::vector<QueuedItem> batch;
    {
        SrwLockGuard guard(self->lock);
        if (self->closed) return;
        batch.swap(self->pending);
    }

    HRESULT batch_hr = S_OK;
    for (auto& item : batch) {
        if (item.kind == QueuedItem::Kind::Request) {
            const HRESULT hr = execute_request(item.request, item.file);
            if (SUCCEEDED(batch_hr) && FAILED(hr)) batch_hr = hr;
        } else if (item.kind == QueuedItem::Kind::Status && item.status_array) {
            SrwLockGuard sguard(item.status_array->lock);
            if (item.status_index < item.status_array->slots.size()) {
                item.status_array->slots[item.status_index].complete = true;
                item.status_array->slots[item.status_index].result = batch_hr;
            }
            batch_hr = S_OK;
        }
        release_item_refs(item);
    }
}

void STDMETHODCALLTYPE queue_cancel_tag(QueueObject*, UINT64, UINT64) {}
HANDLE STDMETHODCALLTYPE queue_get_error_event(QueueObject*) { return nullptr; }
void STDMETHODCALLTYPE queue_retrieve_error(QueueObject*, void* record, UINT32 size) {
    if (record && size > 0) std::memset(record, 0, size);
}
void STDMETHODCALLTYPE queue_query(QueueObject*, void* info) { (void)info; }

const QueueVtbl kQueueVtbl = {queue_qi,
                              queue_addref,
                              queue_release,
                              queue_enqueue_request,
                              queue_enqueue_status,
                              queue_enqueue_signal,
                              queue_submit,
                              queue_cancel_tag,
                              queue_close,
                              queue_get_error_event,
                              queue_retrieve_error,
                              queue_query};

// ---- IDStorageFactoryX ---------------------------------------------------------------------------------------

struct FactoryObject;
struct FactoryVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(FactoryObject* self, const IID* riid, void** ppv);
    ULONG(STDMETHODCALLTYPE* AddRef)(FactoryObject* self);
    ULONG(STDMETHODCALLTYPE* Release)(FactoryObject* self);
    HRESULT(STDMETHODCALLTYPE* CreateQueueSlot3)(FactoryObject* self, const QueueDescX* desc, const IID* riid,
                                                 void** ppv);
    HRESULT(STDMETHODCALLTYPE* OpenFile)(FactoryObject* self, const wchar_t* path, const IID* riid, void** ppv);
    HRESULT(STDMETHODCALLTYPE* CreateStatusArray)(FactoryObject* self, UINT32 capacity, const char* name,
                                                  const IID* riid, void** ppv);
    void(STDMETHODCALLTYPE* SetDebugFlags)(FactoryObject* self, UINT32 flags);
    HRESULT(STDMETHODCALLTYPE* SetCpuAffinity)(FactoryObject* self, UINT64 affinity);
    HRESULT(STDMETHODCALLTYPE* SetStagingBufferSize)(FactoryObject* self, UINT32 size);
    HRESULT(STDMETHODCALLTYPE* SetDisableBypassIO)(FactoryObject* self, BOOL disable);
    HRESULT(STDMETHODCALLTYPE* CreateQueue)(FactoryObject* self, const QueueDescX* desc, const IID* riid, void** ppv);
};

struct FactoryObject {
    const FactoryVtbl* vtbl = nullptr;
    std::atomic<ULONG> refs{1};
    std::atomic<uint32_t> staging_buffer_size{0};
    std::atomic<uint32_t> debug_flags{0};
};

HRESULT STDMETHODCALLTYPE factory_qi(FactoryObject* self, const IID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!riid) return E_INVALIDARG;
    if (guid_equal(*riid, kIidUnknown) || guid_equal(*riid, kIidFactoryX) || guid_equal(*riid, kIidFactoryPc)) {
        self->vtbl->AddRef(self);
        *ppv = self;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE factory_addref(FactoryObject* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE factory_release(FactoryObject* self) {
    const ULONG rem = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}

HRESULT STDMETHODCALLTYPE factory_open_file(FactoryObject*, const wchar_t* path, const IID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!path || path[0] == L'\0' || !riid) return E_INVALIDARG;

    unsigned long err = ERROR_SUCCESS;
    void* raw = rt::fs::open_read_only(path, err);
    if (raw == INVALID_HANDLE_VALUE) {
        return HRESULT_FROM_WIN32(err ? err : ERROR_FILE_NOT_FOUND);
    }

    LARGE_INTEGER file_size{};
    if (!::GetFileSizeEx(static_cast<HANDLE>(raw), &file_size) || file_size.QuadPart < 0) {
        const DWORD size_err = ::GetLastError();
        ::CloseHandle(static_cast<HANDLE>(raw));
        return HRESULT_FROM_WIN32(size_err ? size_err : ERROR_GEN_FAILURE);
    }

    FileObject* file = new (std::nothrow) FileObject{};
    if (!file) {
        ::CloseHandle(static_cast<HANDLE>(raw));
        return E_OUTOFMEMORY;
    }
    file->vtbl = &kFileVtbl;
    file->handle = static_cast<HANDLE>(raw);
    file->size = static_cast<uint64_t>(file_size.QuadPart);

    const HRESULT hr = file->vtbl->QueryInterface(file, riid, ppv);
    file->vtbl->Release(file);
    return hr;
}

HRESULT STDMETHODCALLTYPE factory_create_status_array(FactoryObject*, UINT32 capacity, const char*, const IID* riid,
                                                      void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!riid || capacity == 0) return E_INVALIDARG;

    StatusArrayObject* arr = new (std::nothrow) StatusArrayObject{};
    if (!arr) return E_OUTOFMEMORY;
    arr->vtbl = &kStatusArrayVtbl;
    arr->slots.assign(capacity, StatusSlot{true, S_OK});

    const HRESULT hr = arr->vtbl->QueryInterface(arr, riid, ppv);
    arr->vtbl->Release(arr);
    return hr;
}

void STDMETHODCALLTYPE factory_set_debug_flags(FactoryObject* self, UINT32 flags) {
    self->debug_flags.store(flags, std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE factory_set_cpu_affinity(FactoryObject*, UINT64) { return S_OK; }

HRESULT STDMETHODCALLTYPE factory_set_staging_size(FactoryObject* self, UINT32 size) {
    if (size == 0) return E_INVALIDARG;
    self->staging_buffer_size.store(size, std::memory_order_relaxed);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE factory_set_disable_bypass(FactoryObject*, BOOL) { return S_OK; }

HRESULT STDMETHODCALLTYPE factory_create_queue(FactoryObject*, const QueueDescX* desc, const IID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!desc || !riid || desc->Capacity == 0) return E_INVALIDARG;

    QueueObject* queue = new (std::nothrow) QueueObject{};
    if (!queue) return E_OUTOFMEMORY;
    queue->vtbl = &kQueueVtbl;
    queue->capacity = desc->Capacity;
    queue->pending.reserve(desc->Capacity);

    const HRESULT hr = queue->vtbl->QueryInterface(queue, riid, ppv);
    queue->vtbl->Release(queue);
    return hr;
}

const FactoryVtbl kFactoryVtbl = {factory_qi,
                                  factory_addref,
                                  factory_release,
                                  factory_create_queue,
                                  factory_open_file,
                                  factory_create_status_array,
                                  factory_set_debug_flags,
                                  factory_set_cpu_affinity,
                                  factory_set_staging_size,
                                  factory_set_disable_bypass,
                                  factory_create_queue};

// ---- Entrypoint ----------------------------------------------------------------------------------------------

HRESULT WINAPI dstorage_get_factory(const IID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!riid) return E_INVALIDARG;

    FactoryObject* factory = new (std::nothrow) FactoryObject{};
    if (!factory) return E_OUTOFMEMORY;
    factory->vtbl = &kFactoryVtbl;

    const HRESULT hr = factory->vtbl->QueryInterface(factory, riid, ppv);
    factory->vtbl->Release(factory);
    return hr;
}

}  // namespace
}  // namespace rt::dstorage

#define DSTORAGE_EXPORTS(dll)                                                                        \
    {dll, "DStorageGetFactory", reinterpret_cast<void*>(&rt::dstorage::dstorage_get_factory)}

SHIM_TABLE(dstorage, DSTORAGE_EXPORTS("xcrdapi.dll"), DSTORAGE_EXPORTS("dstorage_xs.dll"),
           DSTORAGE_EXPORTS("dstorage.dll"));
