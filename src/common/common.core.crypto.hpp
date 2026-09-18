// ==============================================================================
// 币安 BTC/ETH 3分钟量化交易系统 - 加密工具
// ==============================================================================
// @file    src/common/common.core.crypto.hpp
// @module  common
// @type    core
// @name    crypto
// @version 1.0.1
// @brief   基于 OpenSSL 3.0 的哈希、HMAC、AES-GCM、随机数、编码工具
//          已修复 25 类运行时问题
//
// 设计原则:
//   - 高层 API：使用 EVP_ 系列，避免弃用函数
//   - 错误处理：所有函数返回 Result<T>，附带 OpenSSL 错误
//   - 线程安全：call_once 初始化，ERR_error_string_n 替代
//   - 内存安全：RAII 管理 OpenSSL 上下文
//   - 敏感清零：OPENSSL_cleanse 清理密钥和明文
//   - 恒定时间：CRYPTO_memcmp 防止时序攻击
//   - 强类型：std::array 固定长度，std::span 避免拷贝
//   - 认证加密：AES-GCM 提供机密性和完整性
// ==============================================================================

#ifndef QUANT_COMMON_CORE_CRYPTO_HPP
#define QUANT_COMMON_CORE_CRYPTO_HPP

// ==============================================================================
// 标准库
// ==============================================================================
#include <array>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ==============================================================================
// OpenSSL
// ==============================================================================
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

// ==============================================================================
// 项目
// ==============================================================================
#include "common/common.core.error.hpp"
#include "common/common.core.logger.hpp"

namespace quant {
namespace crypto {

// ==============================================================================
// 常量
// ==============================================================================
inline constexpr std::size_t kSha256Size = 32;
inline constexpr std::size_t kSha512Size = 64;
inline constexpr std::size_t kHmacSha256Size = 32;
inline constexpr std::size_t kAesGcmIvSize = 12;
inline constexpr std::size_t kAesGcmTagSize = 16;

// ==============================================================================
// 内部：OpenSSL 初始化与错误
// ==============================================================================
namespace detail {

inline void ensure_openssl_initialized() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        OPENSSL_init_crypto(
            OPENSSL_INIT_LOAD_CRYPTO_STRINGS |
            OPENSSL_INIT_ADD_ALL_CIPHERS |
            OPENSSL_INIT_ADD_ALL_DIGESTS,
            nullptr);
    });
}

[[nodiscard]] inline std::string get_openssl_error() {
    std::string err;
    unsigned long e;
    char buf[256];
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof(buf));
        if (!err.empty()) err += "; ";
        err += buf;
    }
    return err;
}

}  // namespace detail

// ==============================================================================
// 安全清零
// ==============================================================================
inline void secure_clear(void* data, std::size_t len) noexcept {
    if (data && len) {
        OPENSSL_cleanse(data, len);
    }
}

template <typename T, std::size_t N>
inline void secure_clear(std::array<T, N>& arr) noexcept {
    secure_clear(arr.data(), arr.size() * sizeof(T));
}

template <typename T>
inline void secure_clear(std::vector<T>& vec) noexcept {
    if (!vec.empty()) {
        secure_clear(vec.data(), vec.size() * sizeof(T));
    }
}

// ==============================================================================
// 恒定时间比较
// ==============================================================================
[[nodiscard]] inline bool constant_time_compare(
    const void* a, const void* b, std::size_t len) noexcept
{
    if (len == 0) return true;
    if (!a || !b) return false;
    return CRYPTO_memcmp(a, b, len) == 0;
}

[[nodiscard]] inline bool constant_time_compare(
    std::span<const uint8_t> a, std::span<const uint8_t> b) noexcept
{
    if (a.size() != b.size()) return false;
    return constant_time_compare(a.data(), b.data(), a.size());
}

// ==============================================================================
// Hex 编码/解码
// ==============================================================================
[[nodiscard]] inline std::string to_hex(std::span<const uint8_t> data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(data.size() * 2);
    for (std::size_t i = 0; i < data.size(); ++i) {
        out[i * 2]     = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

[[nodiscard]] inline Result<std::vector<uint8_t>> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::UserInputInvalid)
            .with_context("hex", "奇数长度");
    }
    auto decode_nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = decode_nibble(hex[i]);
        const int lo = decode_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::UserInputInvalid)
                .with_context("hex", std::string(hex));
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

// ==============================================================================
// Base64 编码/解码
// ==============================================================================
[[nodiscard]] inline std::string to_base64(std::span<const uint8_t> data) {
    if (data.empty()) return {};
    if (data.size() > static_cast<std::size_t>(INT_MAX)) {
        return {};  // 超大输入不支持
    }
    const int encoded_len = 4 * ((static_cast<int>(data.size()) + 2) / 3);
    std::string out;
    out.resize(static_cast<std::size_t>(encoded_len));
    const int n = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()),
        data.data(),
        static_cast<int>(data.size()));
    if (n < 0) return {};
    out.resize(static_cast<std::size_t>(n));
    return out;
}

[[nodiscard]] inline Result<std::vector<uint8_t>> from_base64(std::string_view b64) {
    // 去除空白
    std::string cleaned;
    cleaned.reserve(b64.size());
    for (char c : b64) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            cleaned.push_back(c);
        }
    }

    if (cleaned.empty()) return std::vector<uint8_t>{};

    if (cleaned.size() % 4 != 0) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::UserInputInvalid)
            .with_context("base64", "长度不是4的倍数");
    }

    if (cleaned.size() > static_cast<std::size_t>(INT_MAX)) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::UserInputInvalid)
            .with_context("base64", "输入过大");
    }

    std::vector<uint8_t> out;
    out.resize(cleaned.size() / 4 * 3);

    const int n = EVP_DecodeBlock(
        out.data(),
        reinterpret_cast<const unsigned char*>(cleaned.data()),
        static_cast<int>(cleaned.size()));

    if (n < 0) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::UserInputInvalid)
            .with_context("base64", "解码失败");
    }

    std::size_t final_len = static_cast<std::size_t>(n);
    // 调整填充
    if (cleaned.size() >= 1 && cleaned[cleaned.size() - 1] == '=') {
        --final_len;
    }
    if (cleaned.size() >= 2 && cleaned[cleaned.size() - 2] == '=') {
        --final_len;
    }
    out.resize(final_len);
    return out;
}

// ==============================================================================
// SHA-256
// ==============================================================================
[[nodiscard]] inline Result<std::array<uint8_t, kSha256Size>>
sha256(std::span<const uint8_t> data) {
    detail::ensure_openssl_initialized();

    std::array<uint8_t, kSha256Size> digest{};
    unsigned int len = 0;

    if (EVP_Digest(data.data(), data.size(),
                   digest.data(), &len,
                   EVP_sha256(), nullptr) != 1) {
        return QUANT_ERR_T(std::array<uint8_t, kSha256Size>,
                           ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }

    if (len != kSha256Size) {
        return QUANT_ERR_T(std::array<uint8_t, kSha256Size>,
                           ErrorCode::InternalUnknown)
            .with_context("sha256", "长度错误");
    }

    return digest;
}

// ==============================================================================
// SHA-512
// ==============================================================================
[[nodiscard]] inline Result<std::array<uint8_t, kSha512Size>>
sha512(std::span<const uint8_t> data) {
    detail::ensure_openssl_initialized();

    std::array<uint8_t, kSha512Size> digest{};
    unsigned int len = 0;

    if (EVP_Digest(data.data(), data.size(),
                   digest.data(), &len,
                   EVP_sha512(), nullptr) != 1) {
        return QUANT_ERR_T(std::array<uint8_t, kSha512Size>,
                           ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }

    if (len != kSha512Size) {
        return QUANT_ERR_T(std::array<uint8_t, kSha512Size>,
                           ErrorCode::InternalUnknown)
            .with_context("sha512", "长度错误");
    }

    return digest;
}

// ==============================================================================
// HMAC-SHA256
// ==============================================================================
[[nodiscard]] inline Result<std::array<uint8_t, kHmacSha256Size>>
hmac_sha256(std::span<const uint8_t> key, std::span<const uint8_t> data) {
    detail::ensure_openssl_initialized();

    if (key.size() > static_cast<std::size_t>(INT_MAX) ||
        data.size() > static_cast<std::size_t>(INT_MAX)) {
        return QUANT_ERR_T(std::array<uint8_t, kHmacSha256Size>,
                           ErrorCode::UserInputInvalid)
            .with_context("hmac", "输入过大");
    }

    std::array<uint8_t, kHmacSha256Size> digest{};
    unsigned int len = kHmacSha256Size;

    if (HMAC(EVP_sha256(),
             key.data(), static_cast<int>(key.size()),
             data.data(), data.size(),
             digest.data(), &len) == nullptr) {
        return QUANT_ERR_T(std::array<uint8_t, kHmacSha256Size>,
                           ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }

    if (len != kHmacSha256Size) {
        return QUANT_ERR_T(std::array<uint8_t, kHmacSha256Size>,
                           ErrorCode::InternalUnknown)
            .with_context("hmac", "长度错误");
    }

    return digest;
}

// ==============================================================================
// 安全随机字节
// ==============================================================================
[[nodiscard]] inline Result<std::vector<uint8_t>>
secure_random_bytes(std::size_t len) {
    detail::ensure_openssl_initialized();

    if (len > static_cast<std::size_t>(INT_MAX)) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::UserInputInvalid)
            .with_context("random", "长度过大");
    }

    std::vector<uint8_t> out(len);
    if (len > 0 && RAND_bytes(out.data(), static_cast<int>(len)) != 1) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }
    return out;
}

// ==============================================================================
// AES-GCM 加密结果
// ==============================================================================
struct AesGcmResult {
    std::vector<uint8_t> ciphertext;
    std::array<uint8_t, kAesGcmTagSize> tag{};
    std::array<uint8_t, kAesGcmIvSize> iv{};
};

// ==============================================================================
// AES-GCM 加密
// ==============================================================================
// 密钥长度: 16 (AES-128), 24 (AES-192), 32 (AES-256)
// AAD: 附加认证数据，可为空
// 返回: 密文 + 16字节 tag + 12字节随机 IV
[[nodiscard]] inline Result<AesGcmResult>
aes_gcm_encrypt(std::span<const uint8_t> plaintext,
                std::span<const uint8_t> key,
                std::span<const uint8_t> aad = {}) {
    detail::ensure_openssl_initialized();

    if (key.size() != 16 && key.size() != 24 && key.size() != 32) {
        return QUANT_ERR_T(AesGcmResult, ErrorCode::UserInputInvalid)
            .with_context("key_size", std::to_string(key.size()));
    }

    if (plaintext.size() > static_cast<std::size_t>(INT_MAX) ||
        aad.size() > static_cast<std::size_t>(INT_MAX)) {
        return QUANT_ERR_T(AesGcmResult, ErrorCode::UserInputInvalid)
            .with_context("aes_gcm", "输入过大");
    }

    AesGcmResult result;

    // 生成随机 IV
    if (RAND_bytes(result.iv.data(), static_cast<int>(result.iv.size())) != 1) {
        return QUANT_ERR_T(AesGcmResult, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }

    using CtxPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    CtxPtr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) {
        return QUANT_ERR_T(AesGcmResult, ErrorCode::SystemOutOfMemory);
    }

    const EVP_CIPHER* cipher = nullptr;
    switch (key.size()) {
        case 16: cipher = EVP_aes_128_gcm(); break;
        case 24: cipher = EVP_aes_192_gcm(); break;
        case 32: cipher = EVP_aes_256_gcm(); break;
    }

    if (EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr) != 1) {
        return QUANT_ERR_T(AesGcmResult, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(result.iv.size()), nullptr) != 1) {
        return QUANT_ERR_T(AesGcmResult, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
                           key.data(), result.iv.data()) != 1) {
        return QUANT_ERR_T(AesGcmResult, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }

    int len = 0;
    if (!aad.empty()) {
        if (EVP_EncryptUpdate(ctx.get(), nullptr, &len,
                              aad.data(), static_cast<int>(aad.size())) != 1) {
            return QUANT_ERR_T(AesGcmResult, ErrorCode::InternalUnknown)
                .with_context("openssl", detail::get_openssl_error());
        }
    }

    result.ciphertext.resize(plaintext.size());
    int out_len = 0;
    if (EVP_EncryptUpdate(ctx.get(), result.ciphertext.data(), &out_len,
                          plaintext.data(), static_cast<int>(plaintext.size())) != 1) {
        return QUANT_ERR_T(AesGcmResult, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx.get(),
                            result.ciphertext.data() + out_len,
                            &final_len) != 1) {
        return QUANT_ERR_T(AesGcmResult, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }
    result.ciphertext.resize(static_cast<std::size_t>(out_len + final_len));

    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(result.tag.size()),
                            result.tag.data()) != 1) {
        return QUANT_ERR_T(AesGcmResult, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }

    return result;
}

// ==============================================================================
// AES-GCM 解密
// ==============================================================================
[[nodiscard]] inline Result<std::vector<uint8_t>>
aes_gcm_decrypt(std::span<const uint8_t> ciphertext,
                std::span<const uint8_t> key,
                std::span<const uint8_t> iv,
                std::span<const uint8_t> tag,
                std::span<const uint8_t> aad = {}) {
    detail::ensure_openssl_initialized();

    if (key.size() != 16 && key.size() != 24 && key.size() != 32) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::UserInputInvalid)
            .with_context("key_size", std::to_string(key.size()));
    }
    if (iv.size() != kAesGcmIvSize) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::UserInputInvalid)
            .with_context("iv_size", std::to_string(iv.size()));
    }
    if (tag.size() != kAesGcmTagSize) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::UserInputInvalid)
            .with_context("tag_size", std::to_string(tag.size()));
    }
    if (ciphertext.size() > static_cast<std::size_t>(INT_MAX) ||
        aad.size() > static_cast<std::size_t>(INT_MAX)) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::UserInputInvalid)
            .with_context("aes_gcm", "输入过大");
    }

    using CtxPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    CtxPtr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::SystemOutOfMemory);
    }

    const EVP_CIPHER* cipher = nullptr;
    switch (key.size()) {
        case 16: cipher = EVP_aes_128_gcm(); break;
        case 24: cipher = EVP_aes_192_gcm(); break;
        case 32: cipher = EVP_aes_256_gcm(); break;
    }

    if (EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr) != 1) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(iv.size()), nullptr) != 1) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                           key.data(), iv.data()) != 1) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }

    int len = 0;
    if (!aad.empty()) {
        if (EVP_DecryptUpdate(ctx.get(), nullptr, &len,
                              aad.data(), static_cast<int>(aad.size())) != 1) {
            return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::InternalUnknown)
                .with_context("openssl", detail::get_openssl_error());
        }
    }

    std::vector<uint8_t> plaintext(ciphertext.size());
    int out_len = 0;
    if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &out_len,
                          ciphertext.data(),
                          static_cast<int>(ciphertext.size())) != 1) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }

    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(tag.size()),
                            const_cast<uint8_t*>(tag.data())) != 1) {
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::InternalUnknown)
            .with_context("openssl", detail::get_openssl_error());
    }

    int final_len = 0;
    const int ret = EVP_DecryptFinal_ex(ctx.get(),
                                        plaintext.data() + out_len,
                                        &final_len);
    if (ret != 1) {
        // 认证失败：清零并返回
        secure_clear(plaintext);
        return QUANT_ERR_T(std::vector<uint8_t>, ErrorCode::UserInputInvalid)
            .with_context("aes_gcm", "认证失败");
    }

    plaintext.resize(static_cast<std::size_t>(out_len + final_len));
    return plaintext;
}

}  // namespace crypto
}  // namespace quant

#endif  // QUANT_COMMON_CORE_CRYPTO_HPP
