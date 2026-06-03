// ============================================================
// 传输任务管理器 - 实现
// ============================================================

#include "transfer/transfer_manager.h"
#include "common/utils.h"
#include <iostream>
#include <sys/stat.h>

TransferManager::TransferManager()
    : m_receiver(nullptr)
{
}

TransferManager::~TransferManager() {
}

void TransferManager::add_task(const TransferTask& task) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tasks[task.meta.file_id] = task;
}

bool TransferManager::get_task(const std::string& file_id, TransferTask& out_task) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(file_id);
    if (it != m_tasks.end()) {
        out_task = it->second;
        return true;
    }
    return false;
}

std::vector<TransferTask> TransferManager::get_all_tasks() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<TransferTask> result;
    result.reserve(m_tasks.size());
    for (const auto& pair : m_tasks) {
        result.push_back(pair.second);
    }
    return result;
}

bool TransferManager::pause_task(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(file_id);
    if (it == m_tasks.end()) return false;
    it->second.state = TransferState::PAUSED;
    return true;
}

bool TransferManager::resume_task(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(file_id);
    if (it == m_tasks.end()) return false;
    if (it->second.state == TransferState::PAUSED) {
        it->second.state = TransferState::TRANSFERRING;
    }
    return true;
}

bool TransferManager::cancel_task(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(file_id);
    if (it == m_tasks.end()) return false;
    it->second.state = TransferState::CANCELLED;
    return true;
}

void TransferManager::update_progress(const std::string& file_id,
                                       uint32_t progress_chunk,
                                       uint64_t bytes_sent,
                                       double speed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(file_id);
    if (it == m_tasks.end()) return;
    it->second.progress_chunk = progress_chunk;
    it->second.bytes_sent = bytes_sent;
    it->second.speed = speed;
}

void TransferManager::mark_complete(const std::string& file_id, bool success) {
    std::unique_lock<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(file_id);
    if (it == m_tasks.end()) return;
    it->second.state = success ? TransferState::COMPLETED : TransferState::FAILED;

    if (m_task_complete_cb) {
        TransferTask t = it->second;
        lock.unlock();
        m_task_complete_cb(t);
    }
}

uint32_t TransferManager::get_resume_chunk(const std::string& filename, uint64_t file_size) {
    // 检查是否有临时文件
    std::string temp_path = filename + ".tmp";
    struct stat st;
    if (stat(temp_path.c_str(), &st) == 0 && st.st_size > 0) {
        // 临时文件存在, 计算已接收的分片数
        uint64_t received_bytes = static_cast<uint64_t>(st.st_size);
        if (received_bytes >= file_size) {
            return 0;  // 文件已完整
        }
        // 计算已连续接收的分片数
        uint32_t received_chunks = static_cast<uint32_t>(received_bytes / Defaults::CHUNK_SIZE);
        return received_chunks;
    }
    return 0;  // 全新传输
}

void TransferManager::set_on_task_update(TaskUpdateCallback callback) {
    m_task_update_cb = std::move(callback);
}

void TransferManager::set_on_task_complete(TaskUpdateCallback callback) {
    m_task_complete_cb = std::move(callback);
}
