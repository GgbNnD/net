// ============================================================
// 最小HTTP服务器
// 功能: 提供Web界面和REST API
// ============================================================
#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <map>
#include <cstdint>
#include "common/platform.h"

/**
 * @class HttpServer
 * @brief 最小HTTP服务器
 *
 * 功能:
 * - 提供静态文件服务 (HTML/CSS/JS)
 * - REST API 路由
 * - 支持长轮询 (用于实时设备列表更新)
 */
class HttpServer {
public:
    HttpServer(uint16_t port = 8891);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start();
    void stop();
    bool is_running() const { return m_running; }

    /**
     * @brief 注册 GET 路由处理函数
     * @param path    URL路径 (如 "/api/devices")
     * @param handler 处理函数: 返回 JSON 字符串
     */
    void on_get(const std::string& path,
                std::function<std::string()> handler);

    /**
     * @brief 注册 POST 路由处理函数
     * @param path    URL路径
     * @param handler 处理函数: 接收 body 字符串, 返回 JSON 字符串
     */
    void on_post(const std::string& path,
                 std::function<std::string(const std::string& body)> handler);

    /**
     * @brief 设置静态文件根目录
     */
    void set_static_dir(const std::string& dir) { m_static_dir = dir; }

private:
    uint16_t m_port;
    std::string m_static_dir;
    SOCKET_FD m_listen_socket;
    std::atomic<bool> m_running;
    std::thread m_accept_thread;
    std::vector<std::thread> m_handler_threads;
    std::mutex m_handler_mutex;

    std::map<std::string, std::function<std::string()>> m_get_handlers;
    std::map<std::string, std::function<std::string(const std::string&)>> m_post_handlers;

    bool create_listen_socket();
    void accept_loop();
    void handle_client(SOCKET_FD client_sock);

    /**
     * @brief 解析HTTP请求
     */
    struct HttpRequest {
        std::string method;
        std::string path;
        std::string body;
        std::map<std::string, std::string> headers;
    };

    HttpRequest parse_request(const std::string& raw);

    /**
     * @brief 构建HTTP响应
     */
    std::string build_response(int status_code, const std::string& content_type,
                                const std::string& body);

    /**
     * @brief 读取文件内容
     */
    std::string read_file(const std::string& file_path);

    /**
     * @brief 获取MIME类型
     */
    std::string get_mime_type(const std::string& path);
};
