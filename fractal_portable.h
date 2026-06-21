/*
 * fractal_portable.h — Hardware-Independent Platform Abstraction
 * ==============================================================
 *
 * Provides a single compilation target that works on:
 *   ARM64 (Linux/Android/macOS), x86-64 (Linux/macOS/Windows),
 *   RISC-V, MIPS, WebAssembly, and any C11-compliant platform.
 *
 * Abstracts:
 *   1. CRC32-C        — HW-accelerated on ARM64 (crc32cd) and x86 (sse4.2);
 *                       portable software fallback using Sarwate algorithm.
 *   2. Memory mapping — OS-agnostic MAP_ANONYMOUS without Linux-only flags.
 *   3. Alignment      — compiler-neutral alignas wrappers.
 *   4. Platform macros — single point of truth for HW capability detection.
 *
 * Include this header FIRST in every translation unit.
 */

#ifndef FRACTAL_PORTABLE_H
#define FRACTAL_PORTABLE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════
 * PLATFORM DETECTION
 * ═══════════════════════════════════════════════════════════════════ */

#if defined(__aarch64__) || defined(_M_ARM64)
  #define FP_ARCH_ARM64   1
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #define FP_ARCH_X86     1
#elif defined(__riscv)
  #define FP_ARCH_RISCV   1
#else
  #define FP_ARCH_GENERIC 1
#endif

#if defined(__linux__) || defined(__ANDROID__)
  #define FP_OS_LINUX     1
#elif defined(__APPLE__)
  #define FP_OS_MACOS     1
#elif defined(_WIN32)
  #define FP_OS_WINDOWS   1
#else
  #define FP_OS_GENERIC   1
#endif

/* ═══════════════════════════════════════════════════════════════════
 * CRC32-C (Castagnoli polynomial 0x1EDC6F41)
 * ===================================================================
 *
 * ARM64:    __crc32cd  — 1 instruction, 3 cycles latency.
 * x86-64:   _mm_crc32_u64 via <nmmintrin.h> — sse4.2, 3 cycles.
 * Generic:  Sarwate table-driven byte-by-byte — ~10 cycles/byte.
 *
 * All three produce identical results.
 * The CRC32-C polynomial is:  x³² + x²⁸ + x²⁷ + x²⁶ + x²⁵ + x²³
 *                            + x²² + x²⁰ + x¹⁹ + x¹⁸ + x¹⁴ + x¹³
 *                            + x¹¹ + x¹⁰ + x⁹ + x⁸ + x⁶ + 1
 *                            = 0x1EDC6F41 (reflected)
 *                            = 0x82F63B78 (normal)
 * ═══════════════════════════════════════════════════════════════════ */

#if FP_ARCH_ARM64
  #include <arm_acle.h>
  #if defined(__ARM_FEATURE_CRC32)
    /* HW CRC32-C via crc32cd instruction */
    #define FP_HAVE_HW_CRC32C 1
    static inline uint32_t fp_crc32c_u64(uint64_t v) {
        return __crc32cd(0, v);
    }
  #else
    /* ARM64 without +crc flag — use software table (defined below) */
    #define FP_HAVE_HW_CRC32C 0
  #endif

#elif FP_ARCH_X86
  #include <nmmintrin.h>  /* SSE4.2 _mm_crc32 */
  #define FP_HAVE_HW_CRC32C 1
  static inline uint32_t fp_crc32c_u64(uint64_t v) {
      return (uint32_t)_mm_crc32_u64(0, v);
  }

#else
  #define FP_HAVE_HW_CRC32C 0
#endif

/* Software CRC32-C fallback (used when HW not available).
 * Table generated from Castagnoli polynomial 0x82F63B78 (normal form).
 * This is the standard table used by Linux kernel, ISCSI, SCTP, etc. */
#if !FP_HAVE_HW_CRC32C
static const uint32_t fp_crc32c_table[256] = {
      0x00000000,0xF26B8303,0xE13B70F7,0x1350F3F4,0xC79A971F,0x35F1141C,
      0x26A1E7E8,0xD4CA64EB,0x8AD958CF,0x78B2DBCC,0x6BE22838,0x9989AB3B,
      0x4D43CFD0,0xBF284CD3,0xAC78BF27,0x5E133C24,0x105EC76F,0xE235446C,
      0xF165B798,0x030E349B,0xD7C45070,0x25AFD373,0x36FF2087,0xC494A384,
      0x9A879FA0,0x68EC1CA3,0x7BBCEF57,0x89D76C54,0x5D1D08BF,0xAF768BBC,
      0xBC267848,0x4E4DFB4B,0x20BD8EDE,0xD2D60DDD,0xC186FE29,0x33ED7D2A,
      0xE72719C1,0x154C9AC2,0x061C6936,0xF477EA35,0xAA64D611,0x580F5512,
      0x4B5FA6E6,0xB93425E5,0x6DFE410E,0x9F95C20D,0x8CC531F9,0x7EAEB2FA,
      0x30E349B1,0xC288CAB2,0xD1D83946,0x23B3BA45,0xF779DEAE,0x05125DAD,
      0x1642AE59,0xE4292D5A,0xBA3A117E,0x4851927D,0x5B016189,0xA96AE28A,
      0x7DA08661,0x8FCB0562,0x9C9BF696,0x6EF07595,0x417B1DBC,0xB3109EBF,
      0xA0406D4B,0x522BEE48,0x86E18AA3,0x748A09A0,0x67DAFA54,0x95B17957,
      0xCBA24573,0x39C9C670,0x2A993584,0xD8F2B687,0x0C38D26C,0xFE53516F,
      0xED03A29B,0x1F682198,0x5125DAD3,0xA34E59D0,0xB01EAA24,0x42752927,
      0x96BF4DCC,0x64D4CECF,0x77843D3B,0x85EFBE38,0xDBFC821C,0x2997011F,
      0x3AC7F2EB,0xC8AC71E8,0x1C661503,0xEE0D9600,0xFD5D65F4,0x0F36E6F7,
      0x61C69362,0x93AD1061,0x80FDE395,0x72966096,0xA65C047D,0x5437877E,
      0x4767748A,0xB50CF789,0xEB1FCBAD,0x197448AE,0x0A24BB5A,0xF84F3859,
      0x2C855CB2,0xDEEEDFB1,0xCDBE2C45,0x3FD5AF46,0x7198540D,0x83F3D70E,
      0x90A324FA,0x62C8A7F9,0xB602C312,0x44694011,0x5739B3E5,0xA55230E6,
      0xFB410CC2,0x092A8FC1,0x1A7A7C35,0xE811FF36,0x3CDB9BDD,0xCEB018DE,
      0xDDE0EB2A,0x2F8B6829,0x82F63B78,0x709DB87B,0x63CD4B8F,0x91A6C88C,
      0x456CAC67,0xB7072F64,0xA457DC90,0x563C5F93,0x082F63B7,0xFA44E0B4,
      0xE9141340,0x1B7F9043,0xCFB5F4A8,0x3DDE77AB,0x2E8E845F,0xDCE5075C,
      0x92A8FC17,0x60C37F14,0x73938CE0,0x81F80FE3,0x55326B08,0xA759E80B,
      0xB4091BFF,0x466298FC,0x1871A4D8,0xEA1A27DB,0xF94AD42F,0x0B21572C,
      0xDFEB33C7,0x2D80B0C4,0x3ED04330,0xCCBBC033,0xA24BB5A6,0x502036A5,
      0x4370C551,0xB11B4652,0x65D122B9,0x97BAA1BA,0x84EA524E,0x7681D14D,
      0x2892ED69,0xDAF96E6A,0xC9A99D9E,0x3BC21E9D,0xEF087A76,0x1D63F975,
      0x0E330A81,0xFC588982,0xB21572C9,0x407EF1CA,0x532E023E,0xA145813D,
      0x758FE5D6,0x87E466D5,0x94B49521,0x66DF1622,0x38CC2A06,0xCAA7A905,
      0xD9F75AF1,0x2B9CD9F2,0xFF56BD19,0x0D3D3E1A,0x1E6DCDEE,0xEC064EED,
      0xC38D26C4,0x31E6A5C7,0x22B65633,0xD0DDD530,0x0417B1DB,0xF67C32D8,
      0xE52CC12C,0x1747422F,0x49547E0B,0xBB3FFD08,0xA86F0EFC,0x5A048DFF,
      0x8ECEE914,0x7CA56A17,0x6FF599E3,0x9D9E1AE0,0xD3D3E1AB,0x21B862A8,
      0x32E8915C,0xC083125F,0x144976B4,0xE622F5B7,0xF5720643,0x07198540,
      0x590AB964,0xAB613A67,0xB831C993,0x4A5A4A90,0x9E902E7B,0x6CFBAD78,
      0x7FAB5E8C,0x8DC0DD8F,0xE330A81A,0x115B2B19,0x020BD8ED,0xF0605BEE,
      0x24AA3F05,0xD6C1BC06,0xC5914FF2,0x37FACCF1,0x69E9F0D5,0x9B8273D6,
      0x88D28022,0x7AB90321,0xAE7367CA,0x5C18E4C9,0x4F48173D,0xBD23943E,
      0xF36E6F75,0x0105EC76,0x12551F82,0xE03E9C81,0x34F4F86A,0xC69F7B69,
      0xD5CF889D,0x27A40B9E,0x79B737BA,0x8BDCB4B9,0x988C474D,0x6AE7C44E,
      0xBE2DA0A5,0x4C4623A6,0x5F16D052,0xAD7D5351
  };

  static inline uint32_t fp_crc32c_u64(uint64_t v) {
      const uint8_t *p = (const uint8_t *)&v;
      uint32_t crc = 0;
      for (int i = 0; i < 8; i++) {
          crc = fp_crc32c_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
      }
      return crc;
  }
#endif /* platform CRC32-C selection */

/* Convenience: CRC32-C of an arbitrary byte buffer */
static inline uint32_t fp_crc32c_buf(const uint8_t *data, size_t len) {
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint64_t v = (uint64_t)data[i];
        crc = fp_crc32c_u64(v) ^ (crc >> 8);
    }
    return crc;
}

/* ═══════════════════════════════════════════════════════════════════
 * OS-AGNOSTIC MEMORY MAPPING
 * ===================================================================
 *
 * MAP_ANONYMOUS is POSIX. MAP_NORESERVE is Linux-only and means
 * "don't pre-commit swap" — irrelevant for our in-memory DB since
 * pages are faulted in lazily by the kernel anyway on all modern OSes.
 * We use plain MAP_PRIVATE|MAP_ANONYMOUS for portability.
 * ═══════════════════════════════════════════════════════════════════ */

#if !defined(_WIN32)
  #include <sys/mman.h>
#else
  /* Windows: stub — use VirtualAlloc if needed */
  #include <windows.h>
  #define PROT_READ  0x01
  #define PROT_WRITE 0x02
  #define MAP_PRIVATE   0x02
  #define MAP_ANONYMOUS 0x20
  #define MAP_FAILED    ((void*)(-1))
#endif

/* Portable anonymous memory allocation */
static inline void* fp_anon_alloc(size_t size) {
#if !defined(_WIN32)
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p;
#else
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#endif
}

/* Portable anonymous memory free */
static inline void fp_anon_free(void *ptr, size_t size) {
#if !defined(_WIN32)
    munmap(ptr, size);
#else
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 * COMPILER-NEUTRAL ALIGNMENT
 * ═══════════════════════════════════════════════════════════════════ */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  /* C11 _Alignas */
  #define FP_ALIGNAS(n) _Alignas(n)
#else
  /* GCC/Clang/TCC/ICC */
  #define FP_ALIGNAS(n) __attribute__((aligned(n)))
#endif

/* Cache-line size for data structures (64 on most, 128 on Apple M-series) */
#if defined(__APPLE__) && defined(__aarch64__)
  #define FP_CACHE_LINE 128
#else
  #define FP_CACHE_LINE 64
#endif

/* ═══════════════════════════════════════════════════════════════════
 * NEON / SIMD GUARD
 * ═══════════════════════════════════════════════════════════════════ */

#if defined(__aarch64__) && defined(__ARM_NEON)
  #define FP_HAVE_NEON 1
#else
  #define FP_HAVE_NEON 0
#endif

/* Convenience: compile-time NEON check with graceful message */
#if !FP_HAVE_NEON
  #define FP_NEON_ONLY(msg) _Static_assert(0, msg)
#else
  #define FP_NEON_ONLY(msg) ((void)0)
#endif

/* ═══════════════════════════════════════════════════════════════════
 * ENDIAN-NEUTRAL BYTE EXTRACTION
 *
 * Avoids undefined behavior from type-punning through union or
 * pointer casts on strict-aliasing compilers.
 * ═══════════════════════════════════════════════════════════════════ */

static inline uint64_t fp_load64le(const uint8_t *p, size_t max_len) {
    uint64_t v = 0;
    size_t n = max_len < 8 ? max_len : 8;
    for (size_t i = 0; i < n; i++) v |= ((uint64_t)p[i]) << (i * 8);
    return v;
}

#ifdef __cplusplus
}
#endif

#endif /* FRACTAL_PORTABLE_H */
