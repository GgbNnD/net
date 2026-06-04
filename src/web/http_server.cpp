// ============================================================
// HTTP服务器 - 实现
// ============================================================

#include "web/http_server.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

constexpr int HTTP_LISTEN_BACKLOG = 10;
constexpr size_t RECV_BUF = 16384;

HttpServer::HttpServer(uint16_t port)
    : m_port(port)
    , m_static_dir("src/web/static")
    , m_listen_socket(INVALID_SOCKET_FD)
    , m_running(false)
{
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::on_get(const std::string& path,
                         std::function<std::string()> handler) {
    m_get_handlers[path] = std::move(handler);
}

void HttpServer::on_post(const std::string& path,
                          std::function<std::string(const std::string&,
                                     const std::map<std::string, std::string>&)> handler) {
    m_post_handlers[path] = std::move(handler);
}

bool HttpServer::start() {
    if (m_running.load()) return true;

    if (!create_listen_socket()) return false;

    m_running = true;
    m_accept_thread = std::thread(&HttpServer::accept_loop, this);

    std::cout << "[HTTP] Web服务器启动 (端口 " << m_port << ")" << std::endl;
    return true;
}

void HttpServer::stop() {
    if (!m_running.load()) return;
    m_running = false;

    if (m_listen_socket != INVALID_SOCKET_FD) {
        CLOSE_SOCKET(m_listen_socket);
        m_listen_socket = INVALID_SOCKET_FD;
    }

    if (m_accept_thread.joinable()) m_accept_thread.join();

    {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        for (auto& t : m_handler_threads) {
            if (t.joinable()) t.join();
        }
        m_handler_threads.clear();
    }

    std::cout << "[HTTP] Web服务器已停止" << std::endl;
}

bool HttpServer::create_listen_socket() {
    m_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen_socket == INVALID_SOCKET_FD) return false;

    NetworkUtils::set_reuse_addr(m_listen_socket);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(m_listen_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[HTTP] bind() 失败: " << NetworkUtils::get_last_error_string() << std::endl;
        CLOSE_SOCKET(m_listen_socket);
        m_listen_socket = INVALID_SOCKET_FD;
        return false;
    }

    if (listen(m_listen_socket, HTTP_LISTEN_BACKLOG) < 0) {
        CLOSE_SOCKET(m_listen_socket);
        m_listen_socket = INVALID_SOCKET_FD;
        return false;
    }

    return true;
}

void HttpServer::accept_loop() {
    while (m_running.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(m_listen_socket, &read_fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int sel = select((int)(m_listen_socket + 1), &read_fds, nullptr, nullptr, &tv);
        if (sel < 0) break;
        if (sel == 0) continue;

        SOCKET_FD client_sock = accept(m_listen_socket, nullptr, nullptr);
        if (client_sock == INVALID_SOCKET_FD) break;

        std::thread handler([this, client_sock]() {
            handle_client(client_sock);
        });

        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            m_handler_threads.erase(
                std::remove_if(m_handler_threads.begin(), m_handler_threads.end(),
                    [](std::thread& t) { return !t.joinable(); }),
                m_handler_threads.end()
            );
            m_handler_threads.push_back(std::move(handler));
        }
    }
}

void HttpServer::handle_client(SOCKET_FD client_sock) {
    // 设置读超时 (POST大文件需要更长超时)
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // 第一阶段: 读取HTTP头部 (直到 \r\n\r\n)
    char buf[RECV_BUF];
    std::string request_str;
    while (true) {
        int n = SOCK_RECV(client_sock, buf, RECV_BUF - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        request_str += buf;
        if (request_str.find("\r\n\r\n") != std::string::npos) break;
        if (request_str.size() > 65536) break;
    }

    if (request_str.empty()) {
        CLOSE_SOCKET(client_sock);
        return;
    }

    // 第二阶段: 根据 Content-Length 继续读取剩余 body
    size_t hdr_end = request_str.find("\r\n\r\n");
    if (hdr_end != std::string::npos) {
        // 从头部提取 Content-Length
        size_t content_len = 0;
        size_t cl_pos = request_str.find("Content-Length:");
        if (cl_pos != std::string::npos) {
            cl_pos += 15;
            while (cl_pos < request_str.size() && request_str[cl_pos] == ' ') ++cl_pos;
            size_t end_pos = request_str.find("\r\n", cl_pos);
            if (end_pos != std::string::npos) {
                try { content_len = std::stoul(request_str.substr(cl_pos, end_pos - cl_pos)); }
                catch (...) { content_len = 0; }
            }
        }

        size_t body_start = hdr_end + 4;
        size_t body_received = request_str.size() > body_start ? request_str.size() - body_start : 0;
        while (content_len > 0 && body_received < content_len) {
            size_t to_read = std::min<size_t>(RECV_BUF - 1, content_len - body_received);
            int n = SOCK_RECV(client_sock, buf, static_cast<int>(to_read), 0);
            if (n <= 0) break;
            buf[n] = '\0';
            request_str += buf;
            body_received += n;
        }
    }

    // 解析请求
    HttpRequest req = parse_request(request_str);

    // 构建响应
    std::string response_body;
    std::string content_type = "application/json";
    int status_code = 200;

    if (req.method == "GET") {
        auto it = m_get_handlers.find(req.path);
        if (it != m_get_handlers.end()) {
            response_body = it->second();
        } else {
            // 尝试从静态文件目录提供服务
            std::string file_path = m_static_dir;
            if (req.path == "/" || req.path.empty()) {
                file_path += "/index.html";
            } else {
                file_path += req.path;
            }
            std::string file_content = read_file(file_path);
            if (!file_content.empty()) {
                response_body = file_content;
                content_type = get_mime_type(file_path);
            } else {
                status_code = 404;
                response_body = "{\"error\":\"Not Found\"}";
            }
        }
    } else if (req.method == "POST") {
        auto it = m_post_handlers.find(req.path);
        if (it != m_post_handlers.end()) {
            response_body = it->second(req.body, req.headers);
        } else {
            status_code = 404;
            response_body = "{\"error\":\"Not Found\"}";
        }
    } else if (req.method == "OPTIONS") {
        // CORS 预检请求
        response_body = "";
        content_type = "text/plain";
    } else {
        status_code = 405;
        response_body = "{\"error\":\"Method Not Allowed\"}";
    }

    // 构建HTTP响应
    std::string response = build_response(status_code, content_type, response_body);

    // 发送响应
    SOCK_SEND(client_sock, response.c_str(), static_cast<int>(response.size()), 0);
    CLOSE_SOCKET(client_sock);
}

HttpServer::HttpRequest HttpServer::parse_request(const std::string& raw) {
    HttpRequest req;
    std::istringstream stream(raw);
    std::string line;

    // 解析请求行: GET /path HTTP/1.1
    if (std::getline(stream, line)) {
        // 去除行尾 \r
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::istringstream line_stream(line);
        std::string method, path, version;
        line_stream >> method >> path >> version;

        req.method = method;
        req.path = path;
    }

    // 解析头部
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;

        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            // 去除 value 前导空格
            if (!value.empty() && value[0] == ' ') value = value.substr(1);
            req.headers[key] = value;
        }
    }

    // 读取 body (如果 Content-Length 存在)
    auto it = req.headers.find("Content-Length");
    if (it != req.headers.end()) {
        size_t content_len = std::stoul(it->second);
        // body 在 headers 后的空行之后
        size_t body_start = raw.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            body_start += 4;
            if (body_start < raw.size()) {
                req.body = raw.substr(body_start, std::min(content_len, raw.size() - body_start));
            }
        }
    }

    return req;
}

std::string HttpServer::build_response(int status_code,
                                        const std::string& content_type,
                                        const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status_code << " ";
    switch (status_code) {
        case 200: response << "OK"; break;
        case 404: response << "Not Found"; break;
        case 405: response << "Method Not Allowed"; break;
        default:  response << "Unknown"; break;
    }
    response << "\r\n";

    response << "Content-Type: " << content_type << "; charset=utf-8\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    // CORS 头 (允许跨域访问)
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    response << "Access-Control-Allow-Headers: Content-Type\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;

    return response.str();
}

std::string HttpServer::read_file(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return "";

    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0);

    std::string content(size, '\0');
    file.read(&content[0], size);
    return content;
}

std::string HttpServer::get_mime_type(const std::string& path) {
    if (path.find(".html") != std::string::npos) return "text/html";
    if (path.find(".css")  != std::string::npos) return "text/css";
    if (path.find(".js")   != std::string::npos) return "application/javascript";
    if (path.find(".json") != std::string::npos) return "application/json";
    if (path.find(".png")  != std::string::npos) return "image/png";
    if (path.find(".jpg")  != std::string::npos) return "image/jpeg";
    if (path.find(".ico")  != std::string::npos) return "image/x-icon";
    return "text/plain";
}
