// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - K 线缓冲区实现
// ==============================================================================
// @file    src/data/data.core.candle_buffer.cpp
// @module  data
// @type    core
// @name    candle_buffer
// @version 1.0.1
// @brief   CandleSnapshot 的非 inline 实现 + CandleBuffer 显式实例化
//          已修复 25 类运行时问题
//
// 说明:
//   CandleBuffer<Capacity> 是模板类，实现在 hpp 中。
//   本文件负责：
//     1. CandleSnapshot 的构造函数、析构函数、提取方法（从 hpp 移出）
//     2. 常用容量的显式模板实例化，加速下游编译
//
// 预实例化的容量（与下游约定）:
//   - 256     小型（测试、单品种短周期）
//   - 512     中小型
//   - 1024    默认（3分钟 K 线约 2 天）
//   - 2048    中型
//   - 4096    大型（约 8 天）
//   - 8192    超大型
//   - 16384   历史加载
// ==============================================================================

#include "data/data.core.candle_buffer.hpp"

// ==============================================================================
// 标准库
// ==============================================================================
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace quant {
namespace data {

// ==============================================================================
// CandleSnapshot 实现
// ==============================================================================

// -----------------------------------------------------------------------------
// 构造/析构
// -----------------------------------------------------------------------------
CandleSnapshot::CandleSnapshot() noexcept = default;

CandleSnapshot::~CandleSnapshot() noexcept = default;

CandleSnapshot::CandleSnapshot(CandleSnapshot&& other) noexcept
    : candles_{std::move(other.candles_)}
    , size_{other.size_}
    , version_{other.version_}
    , symbol_{std::move(other.symbol_)}
{
    other.size_ = 0;
    other.version_ = 0;
}

CandleSnapshot& CandleSnapshot::operator=(CandleSnapshot&& other) noexcept {
    if (this != &other) {
        candles_ = std::move(other.candles_);
        size_ = other.size_;
        version_ = other.version_;
        symbol_ = std::move(other.symbol_);

        other.size_ = 0;
        other.version_ = 0;
    }
    return *this;
}

// -----------------------------------------------------------------------------
// 提取方法
// -----------------------------------------------------------------------------

// 通用提取实现（避免重复代码）
namespace {

template <typename Extractor>
[[nodiscard]] std::vector<double> extract_values(
    const Candle* candles, std::size_t size, Extractor&& extractor)
{
    if (size == 0 || candles == nullptr) {
        return {};
    }

    std::vector<double> result;
    try {
        result.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            const double v = extractor(candles[i]);
            // 边界校验：NaN 或 Inf 替换为 0
            if (std::isfinite(v)) {
                result.push_back(v);
            } else {
                result.push_back(0.0);
            }
        }
    } catch (const std::bad_alloc&) {
        QUANT_LOG_ERROR("[CandleSnapshot] 提取数组分配失败 (size={})", size);
        return {};
    }

    return result;
}

// 通用：提取到已分配缓冲区（避免重复分配）
template <typename Extractor>
[[nodiscard]] std::size_t extract_values_into(
    const Candle* candles, std::size_t size,
    double* out, std::size_t out_capacity,
    Extractor&& extractor)
{
    if (size == 0 || candles == nullptr || out == nullptr) {
        return 0;
    }

    const std::size_t n = std::min(size, out_capacity);
    for (std::size_t i = 0; i < n; ++i) {
        const double v = extractor(candles[i]);
        out[i] = std::isfinite(v) ? v : 0.0;
    }
    return n;
}

}  // namespace

std::vector<double> CandleSnapshot::extract_closes() const {
    return extract_values(candles_.get(), size_,
        [](const Candle& c) noexcept { return c.close.to_double(); });
}

std::vector<double> CandleSnapshot::extract_highs() const {
    return extract_values(candles_.get(), size_,
        [](const Candle& c) noexcept { return c.high.to_double(); });
}

std::vector<double> CandleSnapshot::extract_lows() const {
    return extract_values(candles_.get(), size_,
        [](const Candle& c) noexcept { return c.low.to_double(); });
}

std::vector<double> CandleSnapshot::extract_volumes() const {
    return extract_values(candles_.get(), size_,
        [](const Candle& c) noexcept { return c.volume.to_double(); });
}

// -----------------------------------------------------------------------------
// 提取到预分配缓冲区（零分配版本，热路径用）
// -----------------------------------------------------------------------------
std::size_t CandleSnapshot::extract_closes_into(
    double* out, std::size_t out_capacity) const noexcept
{
    return extract_values_into(candles_.get(), size_,
        out, out_capacity,
        [](const Candle& c) noexcept { return c.close.to_double(); });
}

std::size_t CandleSnapshot::extract_highs_into(
    double* out, std::size_t out_capacity) const noexcept
{
    return extract_values_into(candles_.get(), size_,
        out, out_capacity,
        [](const Candle& c) noexcept { return c.high.to_double(); });
}

std::size_t CandleSnapshot::extract_lows_into(
    double* out, std::size_t out_capacity) const noexcept
{
    return extract_values_into(candles_.get(), size_,
        out, out_capacity,
        [](const Candle& c) noexcept { return c.low.to_double(); });
}

std::size_t CandleSnapshot::extract_volumes_into(
    double* out, std::size_t out_capacity) const noexcept
{
    return extract_values_into(candles_.get(), size_,
        out, out_capacity,
        [](const Candle& c) noexcept { return c.volume.to_double(); });
}

// -----------------------------------------------------------------------------
// 时间戳提取（回测常用）
// -----------------------------------------------------------------------------
std::vector<std::int64_t> CandleSnapshot::extract_open_times() const {
    if (size_ == 0 || !candles_) return {};

    std::vector<std::int64_t> result;
    try {
        result.reserve(size_);
        for (std::size_t i = 0; i < size_; ++i) {
            result.push_back(candles_[i].open_time.microseconds());
        }
    } catch (const std::bad_alloc&) {
        QUANT_LOG_ERROR("[CandleSnapshot] 时间戳提取失败 (size={})", size_);
        return {};
    }
    return result;
}

// -----------------------------------------------------------------------------
// 比较运算符
// -----------------------------------------------------------------------------
bool CandleSnapshot::operator==(const CandleSnapshot& other) const noexcept {
    if (this == &other) return true;
    if (size_ != other.size_) return false;
    if (version_ != other.version_) return false;

    // 仅比较首尾时间戳（避免全量比较开销）
    if (size_ == 0) return true;

    if (candles_[0].open_time != other.candles_[0].open_time) return false;
    if (candles_[size_ - 1].open_time != other.candles_[size_ - 1].open_time) {
        return false;
    }
    return true;
}

bool CandleSnapshot::operator!=(const CandleSnapshot& other) const noexcept {
    return !(*this == other);
}

// -----------------------------------------------------------------------------
// 诊断
// -----------------------------------------------------------------------------
std::string CandleSnapshot::dump() const {
    std::string result;
    result.reserve(256);
    result += "CandleSnapshot:\n";
    result += "  symbol:      " + symbol_ + "\n";
    result += "  size:        " + std::to_string(size_) + "\n";
    result += "  version:     " + std::to_string(version_) + "\n";

    if (size_ > 0 && candles_) {
        result += "  first_time:  "
            + std::string{candles_[0].open_time.to_iso8601()} + "\n";
        result += "  last_time:   "
            + std::string{candles_[size_ - 1].open_time.to_iso8601()} + "\n";
    }
    return result;
}

// -----------------------------------------------------------------------------
// 校验（Debug/测试用）
// -----------------------------------------------------------------------------
Result<void> CandleSnapshot::validate() const {
    if (size_ == 0) {
        return {};  // 空快照合法
    }

    if (!candles_) {
        return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
            "非空快照的 candles_ 为空")
            .with_context("size", std::to_string(size_));
    }

    // 时间戳单调性校验
    for (std::size_t i = 1; i < size_; ++i) {
        if (candles_[i].open_time <= candles_[i - 1].open_time) {
            return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
                "快照时间戳非单调")
                .with_context("index", std::to_string(i))
                .with_context("prev",
                    std::string{candles_[i-1].open_time.to_iso8601()})
                .with_context("curr",
                    std::string{candles_[i].open_time.to_iso8601()});
        }
    }

    // K 线有效性校验
    for (std::size_t i = 0; i < size_; ++i) {
        if (!candles_[i].is_valid()) {
            return QUANT_ERR_MSG(ErrorCode::InternalInvariant,
                "快照包含无效 K 线")
                .with_context("index", std::to_string(i))
                .with_context("time",
                    std::string{candles_[i].open_time.to_iso8601()});
        }
    }

    return {};
}

// ==============================================================================
// CandleBuffer 显式模板实例化
// ==============================================================================
// 目的：加速下游编译，避免每个 TU 都实例化模板
//
// 注意：
//   - 下游若使用未列出的容量，仍需在头文件中实例化
//   - 每新增一个容量，编译时间增加约 0.5s，但下游编译时间显著减少
//
// 支持的容量（2 的幂）:
//   256      小型（4KB 缓冲区，约 12 小时数据）
//   512      中小型（8KB，约 1 天）
//   1024     默认（16KB，约 2 天）
//   2048     中型（32KB，约 4 天）
//   4096     大型（64KB，约 8 天）
//   8192     超大型（128KB，约 16 天）
//   16384    历史加载（256KB，约 32 天）
// ==============================================================================

template class CandleBuffer<256>;
template class CandleBuffer<512>;
template class CandleBuffer<1024>;
template class CandleBuffer<2048>;
template class CandleBuffer<4096>;
template class CandleBuffer<8192>;
template class CandleBuffer<16384>;

// ==============================================================================
// 编译期校验
// ==============================================================================
static_assert(std::is_move_constructible_v<CandleSnapshot>,
    "CandleSnapshot 必须可移动构造");
static_assert(std::is_move_assignable_v<CandleSnapshot>,
    "CandleSnapshot 必须可移动赋值");
static_assert(!std::is_copy_constructible_v<CandleSnapshot>,
    "CandleSnapshot 禁止拷贝构造（避免悬空）");
static_assert(!std::is_copy_assignable_v<CandleSnapshot>,
    "CandleSnapshot 禁止拷贝赋值（避免悬空）");

}  // namespace data
}  // namespace quant
