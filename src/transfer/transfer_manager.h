#pragma once

// ============================================================
// 传输任务管理器
// 功能: 管理并发传输任务, 暂停/恢复/取消, 断点续传协调
// ============================================================

#include "common/types.h"
#include <map>
#include <mutex>
#include <functional>

/**
 * @class TransferManager
 * @brief 传输任务管理器
 *
 * 功能:
 * - 管理所有传输任务 (发送+接收)
 * - 支持暂停/恢复/取消
 * - 断点续传: 接收方检测已有临时文件, 发送方从恢复点开始
 */
class TransferManager {
public:
    TransferManager();
    ~TransferManager();

    TransferManager(const TransferManager&) = delete;
    TransferManager& operator=(const TransferManager&) = delete;

    /**
     * @brief 设置接收方服务 (让管理器知道接收端口)
     */
    void set_receiver(void* receiver) { m_receiver = receiver; }

    /**
     * @brief 添加传输任务
     * @param task 传输任务信息
     */
    void add_task(const TransferTask& task);

    /**
     * @brief 获取指定的传输任务
     * @param file_id 传输任务ID
     * @param out_task 输出: 任务信息
     * @return 找到返回 true
     */
    bool get_task(const std::string& file_id, TransferTask& out_task) const;

    /**
     * @brief 获取所有传输任务
     * @return 任务列表 (包括已完成和传输中的)
     */
    std::vector<TransferTask> get_all_tasks() const;

    /**
     * @brief 暂停传输
     * @param file_id 传输任务ID
     */
    bool pause_task(const std::string& file_id);

    /**
     * @brief 恢复传输
     * @param file_id 传输任务ID
     */
    bool resume_task(const std::string& file_id);

    /**
     * @brief 取消传输
     * @param file_id 传输任务ID
     */
    bool cancel_task(const std::string& file_id);

    /**
     * @brief 更新传输任务进度
     * @param file_id 传输任务ID
     * @param progress_chunk 已完成分片数
     * @param bytes_sent 已发送字节数
     * @param speed 当前速度
     */
    void update_progress(const std::string& file_id, uint32_t progress_chunk,
                          uint64_t bytes_sent, double speed);

    /**
     * @brief 标记传输任务完成
     * @param file_id 传输任务ID
     * @param success 是否成功
     */
    void mark_complete(const std::string& file_id, bool success);

    /**
     * @brief 获取断点续传的起始分片
     * @param filename 文件名
     * @param file_size 预期文件大小
     * @return 从哪个分片开始 (0 = 全新传输)
     */
    uint32_t get_resume_chunk(const std::string& filename, uint64_t file_size);

    /**
     * @brief 设置任务变更回调 (用于Web界面通知)
     */
    using TaskUpdateCallback = std::function<void(const TransferTask& task)>;
    void set_on_task_update(TaskUpdateCallback callback);
    void set_on_task_complete(TaskUpdateCallback callback);

private:
    mutable std::mutex m_mutex;
    std::map<std::string, TransferTask> m_tasks;  // 任务列表
    void* m_receiver;  // 接收方服务指针 (BtTransferReceiver*)

    TaskUpdateCallback m_task_update_cb;
    TaskUpdateCallback m_task_complete_cb;
};
