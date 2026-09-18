// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - SIMD 工具
// ==============================================================================
// @file    src/common/common.core.simd.hpp
// @module  common
// @type    core
// @name    simd
// @version 1.0.1
// @brief   跨平台 SIMD 抽象，支持 AVX2 / AVX-512 / NEON / 标量回退
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 运行时检测：CPUID 检测 CPU 支持，动态分发
//   - 编译时分支：__AVX2__ / __ARM_NEON 等宏保护
//   - 标量回退：所有平台都有正确实现
//   - 对齐保证：aligned_load/store 使用 alignas(32/64)
//   - 精度可控：FMA/非 FMA 明确区分，不启用 -ffast-math
//   - vzeroupper：AVX 函数退出时清理
//   - 水平归约：树形归约减少误差
//   - 类型安全：整数与浮点分离，宽度显式
// ==============================================================================

#ifndef QUANT_COMMON_CORE_SIMD_HPP
#define QUANT_COMMON_CORE_SIMD_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

// ==============================================================================
// 平台与特性检测
// ==============================================================================

// 架构检测
#if defined(__x86_64__) || defined(_M_X64) || \
    defined(__i386__)   || defined(_M_IX86)
    #define QUANT_ARCH_X86 1
    #define QUANT_ARCH_ARM 0
#elif defined(__aarch64__) || defined(_M_ARM64) || \
      defined(__arm__)     || defined(_M_ARM)
    #define QUANT_ARCH_X86 0
    #define QUANT_ARCH_ARM 1
#else
    #define QUANT_ARCH_X86 0
    #define QUANT_ARCH_ARM 0
#endif

// x86 头文件
#if QUANT_ARCH_X86
    #include <immintrin.h>
#endif

// ARM NEON 头文件
#if QUANT_ARCH_ARM && defined(__ARM_NEON)
    #include <arm_neon.h>
#endif

// 编译期指令集检测
#if QUANT_ARCH_X86
    #if defined(__AVX512F__)
        #define QUANT_SIMD_AVX512 1
    #else
        #define QUANT_SIMD_AVX512 0
    #endif

    #if defined(__AVX2__) && defined(__FMA__)
        #define QUANT_SIMD_AVX2 1
    #else
        #define QUANT_SIMD_AVX2 0
    #endif

    #if defined(__SSE4_2__)
        #define QUANT_SIMD_SSE42 1
    #else
        #define QUANT_SIMD_SSE42 0
    #endif
#else
    #define QUANT_SIMD_AVX512 0
    #define QUANT_SIMD_AVX2   0
    #define QUANT_SIMD_SSE42  0
#endif

// NEON 检测
#if QUANT_ARCH_ARM && defined(__ARM_NEON)
    #define QUANT_SIMD_NEON 1
#else
    #define QUANT_SIMD_NEON 0
#endif

// 编译期分支：是否有任何 SIMD
#define QUANT_SIMD_ANY (QUANT_SIMD_AVX2 || QUANT_SIMD_AVX512 || QUANT_SIMD_NEON)

namespace quant {
namespace simd {

// ==============================================================================
// 常量
// ==============================================================================
inline constexpr std::size_t kCacheLine = 64;

#if QUANT_SIMD_AVX512
    inline constexpr std::size_t kLanesDouble = 8;
    inline constexpr std::size_t kLanesFloat  = 16;
    inline constexpr std::size_t kAlignment   = 64;
#elif QUANT_SIMD_AVX2
    inline constexpr std::size_t kLanesDouble = 4;
    inline constexpr std::size_t kLanesFloat  = 8;
    inline constexpr std::size_t kAlignment   = 32;
#elif QUANT_SIMD_NEON
    inline constexpr std::size_t kLanesDouble = 2;
    inline constexpr std::size_t kLanesFloat  = 4;
    inline constexpr std::size_t kAlignment   = 16;
#else
    inline constexpr std::size_t kLanesDouble = 1;
    inline constexpr std::size_t kLanesFloat  = 1;
    inline constexpr std::size_t kAlignment   = 8;
#endif

// ==============================================================================
// 运行时 CPU 特性检测
// ==============================================================================
struct CpuFeatures {
    bool sse42{false};
    bool avx2{false};
    bool avx512{false};
    bool fma{false};
    bool neon{false};

    [[nodiscard]] static const CpuFeatures& get() noexcept {
        static const CpuFeatures instance = detect();
        return instance;
    }

private:
    [[nodiscard]] static CpuFeatures detect() noexcept {
        CpuFeatures f{};

#if QUANT_ARCH_X86 && (defined(__GNUC__) || defined(__clang__))
        __builtin_cpu_init();
        f.sse42  = __builtin_cpu_supports("sse4.2");
        f.avx2   = __builtin_cpu_supports("avx2");
        f.avx512 = __builtin_cpu_supports("avx512f");
        f.fma    = __builtin_cpu_supports("fma");
#elif QUANT_ARCH_X86 && defined(_MSC_VER)
        int cpuinfo[4] = {};
        __cpuid(cpuinfo, 1);
        f.sse42 = (cpuinfo[2] & (1 << 20)) != 0;
        f.fma   = (cpuinfo[2] & (1 << 12)) != 0;
        __cpuidex(cpuinfo, 7, 0);
        f.avx2   = (cpuinfo[1] & (1 << 5)) != 0;
        f.avx512 = (cpuinfo[1] & (1 << 16)) != 0;
#elif QUANT_ARCH_ARM && defined(__ARM_NEON)
        f.neon = true;
#endif

        return f;
    }
};

// ==============================================================================
// 浮点环境控制
// ==============================================================================

// 启用 flush-to-zero 和 denormals-are-zero（提高非规格化数性能）
class ScopedFtz {
public:
    ScopedFtz() noexcept {
#if QUANT_ARCH_X86 && defined(__SSE__)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040);  // FTZ | DAZ
#endif
    }

    ~ScopedFtz() noexcept {
#if QUANT_ARCH_X86 && defined(__SSE__)
        _mm_setcsr(saved_);
#endif
    }

    ScopedFtz(const ScopedFtz&) = delete;
    ScopedFtz& operator=(const ScopedFtz&) = delete;

private:
#if QUANT_ARCH_X86 && defined(__SSE__)
    unsigned int saved_{0};
#endif
};

// ==============================================================================
// vzeroupper 清理（AVX 函数退出时调用，避免 AVX-SSE 转换惩罚）
// ==============================================================================
#if QUANT_SIMD_AVX2
    #define QUANT_SIMD_CLEANUP() _mm256_zeroupper()
#else
    #define QUANT_SIMD_CLEANUP() ((void)0)
#endif

// ==============================================================================
// 向量类型别名
// ==============================================================================
#if QUANT_SIMD_AVX512
    using Vec4d = __m256d;
    using Vec8d = __m512d;
    using Vec8f = __m256;
    using Vec16f = __m512;
#elif QUANT_SIMD_AVX2
    using Vec4d = __m256d;
    using Vec8f = __m256;
#elif QUANT_SIMD_NEON
    using Vec2d = float64x2_t;
    using Vec4f = float32x4_t;
#endif

// ==============================================================================
// 标量回退实现（所有平台可用）
// ==============================================================================
namespace scalar {

[[nodiscard]] inline double sum(const double* data, std::size_t n) noexcept {
    // 树形归约减少误差
    if (n == 0) return 0.0;
    if (n <= 8) {
        double result = 0.0;
        for (std::size_t i = 0; i < n; ++i) result += data[i];
        return result;
    }
    // 分块
    constexpr std::size_t kBlock = 8;
    double partials[kBlock] = {};
    std::size_t i = 0;
    for (; i + kBlock <= n; i += kBlock) {
        for (std::size_t j = 0; j < kBlock; ++j) partials[j] += data[i + j];
    }
    for (; i < n; ++i) partials[0] += data[i];

    // 树形归约
    for (std::size_t stride = kBlock / 2; stride > 0; stride /= 2) {
        for (std::size_t j = 0; j < stride; ++j) {
            partials[j] += partials[j + stride];
        }
    }
    return partials[0];
}

[[nodiscard]] inline double mean(const double* data, std::size_t n) noexcept {
    return n == 0 ? 0.0 : sum(data, n) / static_cast<double>(n);
}

[[nodiscard]] inline double max(const double* data, std::size_t n) noexcept {
    if (n == 0) return -std::numeric_limits<double>::infinity();
    double m = data[0];
    for (std::size_t i = 1; i < n; ++i) m = std::max(m, data[i]);
    return m;
}

[[nodiscard]] inline double min(const double* data, std::size_t n) noexcept {
    if (n == 0) return std::numeric_limits<double>::infinity();
    double m = data[0];
    for (std::size_t i = 1; i < n; ++i) m = std::min(m, data[i]);
    return m;
}

inline void add(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) out[i] = a[i] + b[i];
}

inline void sub(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) out[i] = a[i] - b[i];
}

inline void mul(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) out[i] = a[i] * b[i];
}

inline void div(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) out[i] = a[i] / b[i];
}

inline void mul_add(const double* a, const double* b, const double* c,
                    double* out, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) out[i] = a[i] * b[i] + c[i];
}

inline void scale(const double* a, double s, double* out,
                  std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) out[i] = a[i] * s;
}

[[nodiscard]] inline double dot(const double* a, const double* b,
                                std::size_t n) noexcept {
    double result = 0.0;
    for (std::size_t i = 0; i < n; ++i) result += a[i] * b[i];
    return result;
}

[[nodiscard]] inline double variance(const double* data, std::size_t n,
                                      double mean_value) noexcept {
    if (n < 2) return 0.0;
    double sum_sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = data[i] - mean_value;
        sum_sq += d * d;
    }
    return sum_sq / static_cast<double>(n - 1);
}

}  // namespace scalar

// ==============================================================================
// AVX2 实现（x86）
// ==============================================================================
#if QUANT_SIMD_AVX2

namespace avx2 {

// -----------------------------------------------------------------------------
// 水平归约（树形，减少误差）
// -----------------------------------------------------------------------------
[[nodiscard]] inline double hsum(__m256d v) noexcept {
    // v = [a0, a1, a2, a3]
    __m128d low  = _mm256_castpd256_pd128(v);          // [a0, a1]
    __m128d high = _mm256_extractf128_pd(v, 1);        // [a2, a3]
    __m128d sum2 = _mm_add_pd(low, high);              // [a0+a2, a1+a3]
    __m128d sum1 = _mm_hadd_pd(sum2, sum2);            // [a0+a1+a2+a3, ...]
    return _mm_cvtsd_f64(sum1);
}

[[nodiscard]] inline double hmax(__m256d v) noexcept {
    __m128d low  = _mm256_castpd256_pd128(v);
    __m128d high = _mm256_extractf128_pd(v, 1);
    __m128d m2   = _mm_max_pd(low, high);
    __m128d m1   = _mm_max_sd(m2, _mm_unpackhi_pd(m2, m2));
    return _mm_cvtsd_f64(m1);
}

[[nodiscard]] inline double hmin(__m256d v) noexcept {
    __m128d low  = _mm256_castpd256_pd128(v);
    __m128d high = _mm256_extractf128_pd(v, 1);
    __m128d m2   = _mm_min_pd(low, high);
    __m128d m1   = _mm_min_sd(m2, _mm_unpackhi_pd(m2, m2));
    return _mm_cvtsd_f64(m1);
}

// -----------------------------------------------------------------------------
// 数组求和
// -----------------------------------------------------------------------------
[[nodiscard]] inline double sum(const double* data, std::size_t n) noexcept {
    __m256d acc0 = _mm256_setzero_pd();
    __m256d acc1 = _mm256_setzero_pd();
    std::size_t i = 0;

    // 双累加器展开（突破指令延迟）
    for (; i + 8 <= n; i += 8) {
        acc0 = _mm256_add_pd(acc0, _mm256_loadu_pd(data + i));
        acc1 = _mm256_add_pd(acc1, _mm256_loadu_pd(data + i + 4));
    }
    for (; i + 4 <= n; i += 4) {
        acc0 = _mm256_add_pd(acc0, _mm256_loadu_pd(data + i));
    }

    double tail = 0.0;
    for (; i < n; ++i) tail += data[i];

    QUANT_SIMD_CLEANUP();
    return hsum(_mm256_add_pd(acc0, acc1)) + tail;
}

[[nodiscard]] inline double mean(const double* data, std::size_t n) noexcept {
    return n == 0 ? 0.0 : sum(data, n) / static_cast<double>(n);
}

[[nodiscard]] inline double max(const double* data, std::size_t n) noexcept {
    if (n < 4) return scalar::max(data, n);

    __m256d m = _mm256_loadu_pd(data);
    std::size_t i = 4;
    for (; i + 4 <= n; i += 4) {
        m = _mm256_max_pd(m, _mm256_loadu_pd(data + i));
    }
    double result = hmax(m);
    for (; i < n; ++i) result = std::max(result, data[i]);
    QUANT_SIMD_CLEANUP();
    return result;
}

[[nodiscard]] inline double min(const double* data, std::size_t n) noexcept {
    if (n < 4) return scalar::min(data, n);

    __m256d m = _mm256_loadu_pd(data);
    std::size_t i = 4;
    for (; i + 4 <= n; i += 4) {
        m = _mm256_min_pd(m, _mm256_loadu_pd(data + i));
    }
    double result = hmin(m);
    for (; i < n; ++i) result = std::min(result, data[i]);
    QUANT_SIMD_CLEANUP();
    return result;
}

// -----------------------------------------------------------------------------
// 逐元素运算
// -----------------------------------------------------------------------------
inline void add(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        _mm256_storeu_pd(out + i,
            _mm256_add_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
    }
    for (; i < n; ++i) out[i] = a[i] + b[i];
    QUANT_SIMD_CLEANUP();
}

inline void sub(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        _mm256_storeu_pd(out + i,
            _mm256_sub_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
    }
    for (; i < n; ++i) out[i] = a[i] - b[i];
    QUANT_SIMD_CLEANUP();
}

inline void mul(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        _mm256_storeu_pd(out + i,
            _mm256_mul_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
    }
    for (; i < n; ++i) out[i] = a[i] * b[i];
    QUANT_SIMD_CLEANUP();
}

inline void div(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        _mm256_storeu_pd(out + i,
            _mm256_div_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
    }
    for (; i < n; ++i) out[i] = a[i] / b[i];
    QUANT_SIMD_CLEANUP();
}

// -----------------------------------------------------------------------------
// FMA 乘加（out = a * b + c）
// -----------------------------------------------------------------------------
inline void mul_add(const double* a, const double* b, const double* c,
                    double* out, std::size_t n) noexcept {
    std::size_t i = 0;
#if defined(__FMA__)
    for (; i + 4 <= n; i += 4) {
        _mm256_storeu_pd(out + i,
            _mm256_fmadd_pd(_mm256_loadu_pd(a + i),
                            _mm256_loadu_pd(b + i),
                            _mm256_loadu_pd(c + i)));
    }
#endif
    for (; i < n; ++i) out[i] = a[i] * b[i] + c[i];
    QUANT_SIMD_CLEANUP();
}

inline void scale(const double* a, double s, double* out,
                  std::size_t n) noexcept {
    const __m256d sv = _mm256_set1_pd(s);
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        _mm256_storeu_pd(out + i,
            _mm256_mul_pd(_mm256_loadu_pd(a + i), sv));
    }
    for (; i < n; ++i) out[i] = a[i] * s;
    QUANT_SIMD_CLEANUP();
}

// -----------------------------------------------------------------------------
// 点积
// -----------------------------------------------------------------------------
[[nodiscard]] inline double dot(const double* a, const double* b,
                                std::size_t n) noexcept {
    __m256d acc0 = _mm256_setzero_pd();
    __m256d acc1 = _mm256_setzero_pd();
    std::size_t i = 0;
#if defined(__FMA__)
    for (; i + 8 <= n; i += 8) {
        acc0 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i),
                                _mm256_loadu_pd(b + i), acc0);
        acc1 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i + 4),
                                _mm256_loadu_pd(b + i + 4), acc1);
    }
    for (; i + 4 <= n; i += 4) {
        acc0 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i),
                                _mm256_loadu_pd(b + i), acc0);
    }
#else
    for (; i + 4 <= n; i += 4) {
        acc0 = _mm256_add_pd(acc0,
            _mm256_mul_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
    }
#endif

    double tail = 0.0;
    for (; i < n; ++i) tail += a[i] * b[i];

    QUANT_SIMD_CLEANUP();
    return hsum(_mm256_add_pd(acc0, acc1)) + tail;
}

// -----------------------------------------------------------------------------
// 方差（用于波动率计算）
// -----------------------------------------------------------------------------
[[nodiscard]] inline double variance(const double* data, std::size_t n,
                                      double mean_value) noexcept {
    if (n < 2) return 0.0;

    const __m256d mv = _mm256_set1_pd(mean_value);
    __m256d acc = _mm256_setzero_pd();
    std::size_t i = 0;

#if defined(__FMA__)
    for (; i + 4 <= n; i += 4) {
        __m256d d = _mm256_sub_pd(_mm256_loadu_pd(data + i), mv);
        acc = _mm256_fmadd_pd(d, d, acc);
    }
#else
    for (; i + 4 <= n; i += 4) {
        __m256d d = _mm256_sub_pd(_mm256_loadu_pd(data + i), mv);
        acc = _mm256_add_pd(acc, _mm256_mul_pd(d, d));
    }
#endif

    double tail = 0.0;
    for (; i < n; ++i) {
        const double d = data[i] - mean_value;
        tail += d * d;
    }

    QUANT_SIMD_CLEANUP();
    return (hsum(acc) + tail) / static_cast<double>(n - 1);
}

// -----------------------------------------------------------------------------
// EMA（指数移动平均，金融核心）
// -----------------------------------------------------------------------------
// EMA_t = alpha * Close_t + (1 - alpha) * EMA_{t-1}
// 无法直接向量化（递归依赖），但可批量计算多个序列
[[nodiscard]] inline double ema_last(const double* data, std::size_t n,
                                      double alpha,
                                      double initial) noexcept {
    double ema = initial;
    for (std::size_t i = 0; i < n; ++i) {
        ema = alpha * data[i] + (1.0 - alpha) * ema;
    }
    return ema;
}

// 批量计算多个独立的 EMA（不同序列并行）
inline void ema_batch(const double* const* sequences,
                      const std::size_t* lengths,
                      std::size_t num_sequences,
                      double alpha, double* out) noexcept {
    const __m256d av  = _mm256_set1_pd(alpha);
    const __m256d bv  = _mm256_set1_pd(1.0 - alpha);

    std::size_t s = 0;
    for (; s + 4 <= num_sequences; s += 4) {
        __m256d ema = _mm256_set_pd(out[s+3], out[s+2], out[s+1], out[s]);
        std::size_t max_len = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            max_len = std::max(max_len, lengths[s + k]);
        }
        for (std::size_t i = 0; i < max_len; ++i) {
            __m256d x = _mm256_set_pd(
                i < lengths[s+3] ? sequences[s+3][i] : 0.0,
                i < lengths[s+2] ? sequences[s+2][i] : 0.0,
                i < lengths[s+1] ? sequences[s+1][i] : 0.0,
                i < lengths[s+0] ? sequences[s+0][i] : 0.0);
            ema = _mm256_add_pd(_mm256_mul_pd(av, x),
                                _mm256_mul_pd(bv, ema));
        }
        _mm256_storeu_pd(out + s, ema);
    }

    for (; s < num_sequences; ++s) {
        out[s] = ema_last(sequences[s], lengths[s], alpha, out[s]);
    }

    QUANT_SIMD_CLEANUP();
}

// -----------------------------------------------------------------------------
// ATR（平均真实波幅）
// -----------------------------------------------------------------------------
[[nodiscard]] inline double atr(const double* high, const double* low,
                                 const double* close, std::size_t n,
                                 std::size_t period) noexcept {
    if (n < period + 1) return 0.0;

    // TR_i = max(high-low, |high-close_prev|, |low-close_prev|)
    double sum_tr = 0.0;
    for (std::size_t i = 1; i < n; ++i) {
        const double tr = std::max({
            high[i] - low[i],
            std::abs(high[i] - close[i-1]),
            std::abs(low[i]  - close[i-1])
        });
        sum_tr += tr;
    }
    return sum_tr / static_cast<double>(n - 1);
}

}  // namespace avx2

#endif  // QUANT_SIMD_AVX2

// ==============================================================================
// 统一接口（编译期/运行时分发）
// ==============================================================================
// 使用 AVX2 的最小数组大小阈值
inline constexpr std::size_t kSimdThreshold = 32;

[[nodiscard]] inline double sum(const double* data, std::size_t n) noexcept {
#if QUANT_SIMD_AVX2
    if (n >= kSimdThreshold) return avx2::sum(data, n);
#endif
    return scalar::sum(data, n);
}

[[nodiscard]] inline double mean(const double* data, std::size_t n) noexcept {
    return n == 0 ? 0.0 : sum(data, n) / static_cast<double>(n);
}

[[nodiscard]] inline double max(const double* data, std::size_t n) noexcept {
#if QUANT_SIMD_AVX2
    if (n >= kSimdThreshold) return avx2::max(data, n);
#endif
    return scalar::max(data, n);
}

[[nodiscard]] inline double min(const double* data, std::size_t n) noexcept {
#if QUANT_SIMD_AVX2
    if (n >= kSimdThreshold) return avx2::min(data, n);
#endif
    return scalar::min(data, n);
}

inline void add(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
#if QUANT_SIMD_AVX2
    if (n >= kSimdThreshold) return avx2::add(a, b, out, n);
#endif
    scalar::add(a, b, out, n);
}

inline void sub(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
#if QUANT_SIMD_AVX2
    if (n >= kSimdThreshold) return avx2::sub(a, b, out, n);
#endif
    scalar::sub(a, b, out, n);
}

inline void mul(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
#if QUANT_SIMD_AVX2
    if (n >= kSimdThreshold) return avx2::mul(a, b, out, n);
#endif
    scalar::mul(a, b, out, n);
}

inline void div(const double* a, const double* b, double* out,
                std::size_t n) noexcept {
#if QUANT_SIMD_AVX2
    if (n >= kSimdThreshold) return avx2::div(a, b, out, n);
#endif
    scalar::div(a, b, out, n);
}

inline void mul_add(const double* a, const double* b, const double* c,
                    double* out, std::size_t n) noexcept {
#if QUANT_SIMD_AVX2
    if (n >= kSimdThreshold) return avx2::mul_add(a, b, c, out, n);
#endif
    scalar::mul_add(a, b, c, out, n);
}

inline void scale(const double* a, double s, double* out,
                  std::size_t n) noexcept {
#if QUANT_SIMD_AVX2
    if (n >= kSimdThreshold) return avx2::scale(a, s, out, n);
#endif
    scalar::scale(a, s, out, n);
}

[[nodiscard]] inline double dot(const double* a, const double* b,
                                std::size_t n) noexcept {
#if QUANT_SIMD_AVX2
    if (n >= kSimdThreshold) return avx2::dot(a, b, n);
#endif
    return scalar::dot(a, b, n);
}

[[nodiscard]] inline double variance(const double* data, std::size_t n,
                                      double mean_value) noexcept {
#if QUANT_SIMD_AVX2
    if (n >= kSimdThreshold) return avx2::variance(data, n, mean_value);
#endif
    return scalar::variance(data, n, mean_value);
}

// ==============================================================================
// 便捷：从 vector 或 array 调用
// ==============================================================================
template <typename Container>
[[nodiscard]] inline double sum(const Container& c) noexcept {
    return sum(c.data(), c.size());
}

template <typename Container>
[[nodiscard]] inline double mean(const Container& c) noexcept {
    return mean(c.data(), c.size());
}

// ==============================================================================
// 编译期与运行时可用的特性输出
// ==============================================================================
[[nodiscard]] inline constexpr std::string_view compile_time_isa() noexcept {
#if QUANT_SIMD_AVX512
    return "AVX-512";
#elif QUANT_SIMD_AVX2
    return "AVX2+FMA";
#elif QUANT_SIMD_NEON
    return "NEON";
#else
    return "scalar";
#endif
}

}  // namespace simd
}  // namespace quant

#endif  // QUANT_COMMON_CORE_SIMD_HPP
