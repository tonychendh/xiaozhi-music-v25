#ifndef _MUSIC_PLAYER_H_
#define _MUSIC_PLAYER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class AudioService;

// 设备端音乐播放器：拉取远程 OGG(Opus) 流，流式解码后经喇叭输出。
// 管线：HTTP 流 -> OggDemuxer 解封装 -> esp_opus_dec 解码 -> AudioCodec::OutputData
// 全程只用设备上被验证过的 Opus 解码路径（与 TTS 相同），无 MP3/TLS/重采样依赖。
class MusicPlayer {
public:
    MusicPlayer() = default;
    ~MusicPlayer();
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    // 开始播放。返回 false 表示 URL 非法或任务创建失败。
    bool Start(const std::string& url, const std::string& title, const std::string& artist,
               AudioService& audio_service, std::function<void()> on_finished = nullptr);
    // 停止播放并等待播放任务退出（在播放任务内调用时仅置位）
    void Stop();
    bool IsPlaying() const { return playing_.load(); }
    std::string current_title() const;

private:
    struct TaskArgs;
    static void TaskEntry(void* arg);
    void Run(TaskArgs* args);

    std::atomic<bool> playing_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<TaskHandle_t> task_handle_{nullptr};
    AudioService* audio_service_ = nullptr;
    mutable std::mutex mutex_;
    std::string last_url_;
    int64_t last_finished_ms_ = 0;
    std::string current_title_;
};

#endif  // _MUSIC_PLAYER_H_
