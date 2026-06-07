// ============================================================
// 工具函数实现
// ============================================================

#include "common/utils.h"
#include "common/platform.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace Utils {

// ============================================================
// UUID v4 生成
// 原理: UUID v4 使用随机数填充, 第13字符固定为'4', 第17字符为 '8','9','a','b'
// ============================================================
std::string generate_uuid() {
    // 使用线程安全的随机数生成器
    static thread_local std::mt19937_64 rng(
        std::random_device{}() ^
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    static thread_local std::uniform_int_distribution<int> dist(0, 15);
    static const char* hex_chars = "0123456789abcdef";

    std::string uuid(36, '-');
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            uuid[i] = '-';
        } else if (i == 14) {
            uuid[i] = '4';  // UUID version 4
        } else if (i == 19) {
            uuid[i] = hex_chars[(dist(rng) & 0x3) | 0x8];  // UUID variant 1
        } else {
            uuid[i] = hex_chars[dist(rng)];
        }
    }
    return uuid;
}

// ============================================================
// MD5 算法实现 (RFC 1321)
// 这是一个完整的、独立的MD5实现, 不依赖任何外部库
// 算法步骤: 填充 -> 分块处理 -> 更新ABCD四个状态变量
// ============================================================

namespace {

// MD5的四个初始化常量 (小端序)
constexpr uint32_t MD5_A0 = 0x67452301;
constexpr uint32_t MD5_B0 = 0xefcdab89;
constexpr uint32_t MD5_C0 = 0x98badcfe;
constexpr uint32_t MD5_D0 = 0x10325476;

// MD5四轮运算中每轮的位移量 (共64步)
constexpr uint32_t MD5_S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

// MD5 64个常量K[i] = floor(2^32 * |sin(i+1)|)
constexpr uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

// 左循环移位
inline uint32_t left_rotate(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32 - n));
}

/**
 * @brief MD5上下文结构体
 * 用于分块处理大文件时保存中间状态
 */
struct MD5Context {
    uint32_t state[4];                          // A, B, C, D 四个状态变量
    uint64_t total_bytes;                       // 已处理的总字节数 (用于填充)
    uint8_t  buffer[64];                        // 输入缓冲区 (512位 = 64字节)
    uint32_t buffer_len;                        // 缓冲区中已有数据的长度
};

/**
 * @brief 初始化MD5上下文
 */
void md5_init(MD5Context* ctx) {
    ctx->state[0] = MD5_A0;
    ctx->state[1] = MD5_B0;
    ctx->state[2] = MD5_C0;
    ctx->state[3] = MD5_D0;
    ctx->total_bytes = 0;
    ctx->buffer_len = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

/**
 * @brief 处理一个512位(64字节)的数据块
 * @param state  当前的A/B/C/D状态 (会被更新)
 * @param block  64字节的输入数据块
 *
 * MD5核心算法: 四轮 × 16步 = 64步, 每步执行以下操作:
 *   F(X,Y,Z) / G(X,Y,Z) / H(X,Y,Z) / I(X,Y,Z) 非线性函数
 *   a = b + left_rotate(a + F + X[k] + T[i], s[i])
 */
void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];

    // 将64字节的输入块解析为16个32位小端序字
    uint32_t x[16];
    for (int i = 0; i < 16; ++i) {
        x[i] = (uint32_t)block[i * 4] |
               ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    }

    // 64步MD5主循环
    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;

        if (i < 16) {
            // 第一轮: F(X,Y,Z) = (X & Y) | (~X & Z)
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            // 第二轮: G(X,Y,Z) = (X & Z) | (Y & ~Z)
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            // 第三轮: H(X,Y,Z) = X ^ Y ^ Z
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            // 第四轮: I(X,Y,Z) = Y ^ (X | ~Z)
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }

        uint32_t temp = d;
        d = c;
        c = b;
        b = b + left_rotate(a + f + MD5_K[i] + x[g], MD5_S[i]);
        a = temp;
    }

    // 更新状态
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

/**
 * @brief 更新MD5上下文 (处理新的输入数据)
 * @param ctx   MD5上下文
 * @param data  输入数据
 * @param len   数据长度
 */
void md5_update(MD5Context* ctx, const uint8_t* data, size_t len) {
    ctx->total_bytes += len;

    // 如果缓冲区有残留数据, 先与输入合并填充缓冲区
    if (ctx->buffer_len > 0) {
        uint32_t fill = 64 - ctx->buffer_len;
        if (len < fill) {
            // 不足以填满一个块, 全部复制到缓冲区
            memcpy(ctx->buffer + ctx->buffer_len, data, len);
            ctx->buffer_len += (uint32_t)len;
            return;
        }
        // 填满缓冲区, 处理该块
        memcpy(ctx->buffer + ctx->buffer_len, data, fill);
        md5_transform(ctx->state, ctx->buffer);
        data += fill;
        len -= fill;
        ctx->buffer_len = 0;
    }

    // 处理完整的64字节块
    while (len >= 64) {
        md5_transform(ctx->state, data);
        data += 64;
        len -= 64;
    }

    // 剩余数据存入缓冲区
    if (len > 0) {
        memcpy(ctx->buffer, data, len);
        ctx->buffer_len = (uint32_t)len;
    }
}

/**
 * @brief 完成MD5计算, 进行末尾填充并输出128位哈希值
 * @param ctx   MD5上下文
 * @param digest 输出: 16字节的MD5哈希值
 *
 * MD5填充规则:
 * 1. 在消息末尾添加一个字节 0x80 (二进制 10000000)
 * 2. 填充 0x00 直到消息长度 ≡ 448 (mod 512)
 * 3. 在最后8字节填入原始消息长度的低64位 (小端序)
 */
void md5_final(MD5Context* ctx, uint8_t digest[16]) {
    // 计算填充需要的字节数
    // 原始消息长度 (位)
    uint64_t bit_len = ctx->total_bytes * 8;

    // 填充: 至少1字节(0x80), 最多64字节
    uint8_t padding[64];
    padding[0] = 0x80;
    memset(padding + 1, 0, sizeof(padding) - 1);

    uint32_t pad_len;
    if (ctx->buffer_len < 56) {
        // 当前缓冲区 + 填充 + 8字节长度 = 64 字节
        pad_len = 56 - ctx->buffer_len;
    } else {
        // 需要额外一个完整块
        pad_len = 56 + 64 - ctx->buffer_len;
    }

    md5_update(ctx, padding, pad_len);

    // 追加原始消息长度 (64位小端序)
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) {
        len_bytes[i] = (uint8_t)(bit_len >> (i * 8));
    }
    md5_update(ctx, len_bytes, 8);

    // 输出最终结果 (小端序)
    for (int i = 0; i < 4; ++i) {
        digest[i * 4]     = (uint8_t)(ctx->state[i] & 0xFF);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 8) & 0xFF);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 16) & 0xFF);
        digest[i * 4 + 3] = (uint8_t)((ctx->state[i] >> 24) & 0xFF);
    }
}

/**
 * @brief 将16字节的MD5结果转换为32字符的十六进制字符串
 */
std::string digest_to_hex(const uint8_t digest[16]) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        oss << std::setw(2) << (int)digest[i];
    }
    return oss.str();
}

} // anonymous namespace

// ----------------------------------------------------------
// 文件MD5校验
// ----------------------------------------------------------
std::string md5_file(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    MD5Context ctx;
    md5_init(&ctx);

    // 分块读取文件, 避免一次性载入大文件导致内存不足
    constexpr size_t BUFFER_SIZE = 8192;  // 8KB 读取块
    uint8_t buffer[BUFFER_SIZE];

    while (file.good()) {
        file.read(reinterpret_cast<char*>(buffer), BUFFER_SIZE);
        size_t bytes_read = (size_t)file.gcount();
        if (bytes_read > 0) {
            md5_update(&ctx, buffer, bytes_read);
        }
    }

    uint8_t digest[16];
    md5_final(&ctx, digest);
    return digest_to_hex(digest);
}

// ----------------------------------------------------------
// 内存数据MD5校验
// ----------------------------------------------------------
std::string md5_data(const uint8_t* data, size_t size) {
    MD5Context ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, size);

    uint8_t digest[16];
    md5_final(&ctx, digest);
    return digest_to_hex(digest);
}

// ----------------------------------------------------------
// 获取当前时间戳 (毫秒)
// ----------------------------------------------------------
uint64_t get_timestamp_ms() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
}

// ----------------------------------------------------------
// 获取当前时间格式化字符串
// ----------------------------------------------------------
std::string get_time_string() {
    auto now = std::chrono::system_clock::now();
    time_t t = std::chrono::system_clock::to_time_t(now);

    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return std::string(buf);
}

// ----------------------------------------------------------
// 格式化文件大小
// ----------------------------------------------------------
std::string format_file_size(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_idx < 4) {
        size /= 1024.0;
        ++unit_idx;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << size << " " << units[unit_idx];
    return oss.str();
}

// ----------------------------------------------------------
// 格式化传输速度
// ----------------------------------------------------------
std::string format_speed(double bytes_per_sec) {
    if (bytes_per_sec <= 0) return "0 B/s";
    return format_file_size(static_cast<uint64_t>(bytes_per_sec)) + "/s";
}

// ----------------------------------------------------------
// 计算剩余时间
// ----------------------------------------------------------
std::string format_eta(uint64_t total_bytes, uint64_t transferred_bytes, double speed) {
    if (speed <= 0 || transferred_bytes >= total_bytes) {
        return "正在计算...";
    }

    uint64_t remaining_bytes = total_bytes - transferred_bytes;
    double seconds = static_cast<double>(remaining_bytes) / speed;

    if (seconds < 60) {
        return std::to_string(static_cast<int>(seconds)) + "s";
    } else if (seconds < 3600) {
        int mins = static_cast<int>(seconds / 60);
        int secs = static_cast<int>(seconds) % 60;
        return std::to_string(mins) + "m " + std::to_string(secs) + "s";
    } else {
        int hours = static_cast<int>(seconds / 3600);
        int mins = static_cast<int>(seconds) % 3600 / 60;
        return std::to_string(hours) + "h " + std::to_string(mins) + "m";
    }
}

// ----------------------------------------------------------
// 获取文件名 (去除路径)
// ----------------------------------------------------------
std::string get_filename(const std::string& file_path) {
    // 兼容 Windows 的反斜杠和 Linux 的正斜杠
    size_t pos = file_path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return file_path;
    }
    return file_path.substr(pos + 1);
}

// ----------------------------------------------------------
// 获取文件大小
// ----------------------------------------------------------
uint64_t get_file_size(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return 0;
    }
    return static_cast<uint64_t>(file.tellg());
}

// ----------------------------------------------------------
// 字符串分割
// ----------------------------------------------------------
std::vector<std::string> split_string(const std::string& str, char delim) {
    std::vector<std::string> result;
    std::istringstream stream(str);
    std::string token;
    while (std::getline(stream, token, delim)) {
        if (!token.empty()) {
            result.push_back(token);
        }
    }
    return result;
}

// ----------------------------------------------------------
// Base64 解码
// ----------------------------------------------------------
std::string base64_decode(const std::string& input) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    int i = 0;
    unsigned char char_array_4[4], char_array_3[3];

    for (char c : input) {
        if (c == '=') break;
        auto pos = chars.find(c);
        if (pos == std::string::npos) continue;
        char_array_4[i++] = static_cast<unsigned char>(pos);
        if (i == 4) {
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0x0f) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x03) << 6) + char_array_4[3];
            for (int j = 0; j < 3; j++) output += static_cast<char>(char_array_3[j]);
            i = 0;
        }
    }
    if (i) {
        for (int j = i; j < 4; j++) char_array_4[j] = 0;
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0x0f) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        for (int j = 0; j < i - 1; j++) output += static_cast<char>(char_array_3[j]);
    }
    return output;
}

// ----------------------------------------------------------
// URL 解码
// ----------------------------------------------------------
std::string url_decode(const std::string& input) {
    std::string result;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            int hex = 0;
            std::istringstream iss(input.substr(i + 1, 2));
            if (iss >> std::hex >> hex) {
                result += static_cast<char>(hex);
                i += 2;
            } else {
                result += input[i];
            }
        } else if (input[i] == '+') {
            result += ' ';
        } else {
            result += input[i];
        }
    }
    return result;
}

// ----------------------------------------------------------
// ECDH 密钥交换
// ----------------------------------------------------------

#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/bn.h>

bool generate_ecdh_keypair(std::string& public_key_b64, std::string& private_key_b64) {
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx) return false;

    bool ok = false;
    if (EVP_PKEY_keygen_init(ctx) > 0 &&
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) > 0 &&
        EVP_PKEY_keygen(ctx, &pkey) > 0) {

        BIO* pub_bio = BIO_new(BIO_s_mem());
        BIO* priv_bio = BIO_new(BIO_s_mem());
        if (pub_bio && priv_bio) {
            if (i2d_PUBKEY_bio(pub_bio, pkey) > 0 && i2d_PrivateKey_bio(priv_bio, pkey) > 0) {
                char* pub_data = nullptr;
                char* priv_data = nullptr;
                long pub_len = BIO_get_mem_data(pub_bio, &pub_data);
                long priv_len = BIO_get_mem_data(priv_bio, &priv_data);
                if (pub_len > 0 && priv_len > 0) {
                    // DER → Base64
                    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    public_key_b64.clear();
                    private_key_b64.clear();
                    auto encode = [&](const unsigned char* d, long len, std::string& out) {
                        for (long i = 0; i < len; i += 3) {
                            unsigned char a = d[i];
                            unsigned char b = (i + 1 < len) ? d[i + 1] : 0;
                            unsigned char c = (i + 2 < len) ? d[i + 2] : 0;
                            out += b64[a >> 2];
                            out += b64[((a & 0x03) << 4) | (b >> 4)];
                            out += (i + 1 < len) ? b64[((b & 0x0f) << 2) | (c >> 6)] : '=';
                            out += (i + 2 < len) ? b64[c & 0x3f] : '=';
                        }
                    };
                    encode((unsigned char*)pub_data, pub_len, public_key_b64);
                    encode((unsigned char*)priv_data, priv_len, private_key_b64);
                    ok = true;
                }
            }
            if (pub_bio) BIO_free(pub_bio);
            if (priv_bio) BIO_free(priv_bio);
        }
    }

    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

bool compute_ecdh_shared(const std::string& local_private_b64,
                         const std::string& remote_public_b64,
                         std::string& shared_secret_hex) {
    // Base64 decode
    auto b64_decode = [](const std::string& in) -> std::string {
        static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, bits = -8;
        for (unsigned char c : in) {
            if (c == '=') break;
            auto p = chars.find(c);
            if (p == std::string::npos) continue;
            val = (val << 6) | (int)p;
            bits += 6;
            if (bits >= 0) {
                out += (char)((val >> bits) & 0xff);
                bits -= 8;
            }
        }
        return out;
    };

    std::string priv_der = b64_decode(local_private_b64);
    std::string pub_der = b64_decode(remote_public_b64);

    // Load private key
    const unsigned char* priv_ptr = (const unsigned char*)priv_der.data();
    EVP_PKEY* priv_key = d2i_AutoPrivateKey(nullptr, &priv_ptr, (long)priv_der.size());
    if (!priv_key) return false;

    // Load public key
    const unsigned char* pub_ptr = (const unsigned char*)pub_der.data();
    EVP_PKEY* pub_key = d2i_PUBKEY(nullptr, &pub_ptr, (long)pub_der.size());
    if (!pub_key) {
        EVP_PKEY_free(priv_key);
        return false;
    }

    // Compute shared secret
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(priv_key, nullptr);
    bool ok = false;
    if (ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, pub_key) > 0) {
        size_t secret_len = 0;
        if (EVP_PKEY_derive(ctx, nullptr, &secret_len) > 0) {
            std::vector<unsigned char> secret(secret_len);
            if (EVP_PKEY_derive(ctx, secret.data(), &secret_len) > 0) {
                shared_secret_hex = md5_data(secret.data(), secret_len);
                ok = true;
            }
        }
    }

    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pub_key);
    EVP_PKEY_free(priv_key);
    return ok;
}

// ----------------------------------------------------------
// zlib 压缩/解压缩
// ----------------------------------------------------------

#include <zlib.h>

std::string compress_data(const std::string& input) {
    if (input.empty()) return {};

    uLongf dest_len = compressBound(static_cast<uLong>(input.size()));
    std::vector<Bytef> dest(dest_len);

    if (compress(dest.data(), &dest_len,
                 reinterpret_cast<const Bytef*>(input.data()),
                 static_cast<uLong>(input.size())) != Z_OK) {
        return {};
    }

    return std::string(reinterpret_cast<char*>(dest.data()), dest_len);
}

std::string decompress_data(const std::string& input) {
    if (input.empty()) return {};

    uLongf dest_len = static_cast<uLong>(input.size()) * 4;
    std::vector<Bytef> dest(dest_len);
    int ret;

    while ((ret = uncompress(dest.data(), &dest_len,
                              reinterpret_cast<const Bytef*>(input.data()),
                              static_cast<uLong>(input.size()))) == Z_BUF_ERROR) {
        dest_len *= 2;
        dest.resize(dest_len);
    }

    if (ret != Z_OK) return {};

    return std::string(reinterpret_cast<char*>(dest.data()), dest_len);
}

} // namespace Utils
