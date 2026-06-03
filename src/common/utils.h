#pragma once

// ============================================================
// 工具函数声明
// 功能: UUID生成、MD5校验、时间戳、字符串处理等
// ============================================================

#include <string>
#include <cstdint>
#include <vector>

namespace Utils {

/**
 * @brief 生成UUID v4格式的唯一标识符
 * @return UUID字符串, 格式: "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
 *
 * 用于: 设备ID、传输任务ID等需要全局唯一的标识
 */
std::string generate_uuid();

/**
 * @brief 计算文件的MD5校验值
 * @param file_path 文件完整路径
 * @return MD5哈希值的十六进制字符串 (32个字符), 失败返回空字符串
 *
 * 用于: 传输前计算文件哈希, 传输后验证文件完整性
 */
std::string md5_file(const std::string& file_path);

/**
 * @brief 计算内存数据的MD5校验值
 * @param data 数据指针
 * @param size 数据大小 (字节)
 * @return MD5哈希值的十六进制字符串
 */
std::string md5_data(const uint8_t* data, size_t size);

/**
 * @brief 获取当前时间戳 (毫秒)
 * @return 从Unix纪元至今的毫秒数
 */
uint64_t get_timestamp_ms();

/**
 * @brief 获取当前时间格式化字符串
 * @return 格式: "2025-06-03 14:30:00"
 */
std::string get_time_string();

/**
 * @brief 格式化文件大小为人类可读的字符串
 * @param bytes 字节数
 * @return 如 "1.5 MB", "340 KB", "12 GB"
 */
std::string format_file_size(uint64_t bytes);

/**
 * @brief 格式化传输速度为人类可读的字符串
 * @param bytes_per_sec 每秒字节数
 * @return 如 "10.5 MB/s", "500 KB/s"
 */
std::string format_speed(double bytes_per_sec);

/**
 * @brief 计算传输剩余时间
 * @param total_bytes 总字节数
 * @param transferred_bytes 已传输字节数
 * @param speed 当前速度 (字节/秒)
 * @return 剩余时间字符串, 如 "2m 30s", "正在计算..."
 */
std::string format_eta(uint64_t total_bytes, uint64_t transferred_bytes, double speed);

/**
 * @brief 获取文件不包括路径的名称
 * @param file_path 完整或相对路径
 * @return 纯文件名, 如 "/home/user/file.txt" -> "file.txt"
 */
std::string get_filename(const std::string& file_path);

/**
 * @brief 获取文件大小
 * @param file_path 文件路径
 * @return 文件大小 (字节), 失败返回0
 */
uint64_t get_file_size(const std::string& file_path);

/**
 * @brief 简单的字符串分割
 * @param str  待分割字符串
 * @param delim 分隔符
 * @return 分割后的字符串列表
 */
std::vector<std::string> split_string(const std::string& str, char delim);

} // namespace Utils
