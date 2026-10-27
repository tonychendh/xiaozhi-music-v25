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

// 设备端音乐播放器：直接播放远程音频直链（MP3 流），经喇叭输出。
// 播放管线：HTTP 流 -> esp_audio_codec 简易 MP3 解码 -> 声道/采样率转换 -> AudioCodec::OutputData
// 播放期间通过 AudioService::SetExternalPlaybackActive 保持功放常开。
class MusicPlayer {
public:
    MusicPlayer() = default;
    ~MusicPlayer();
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    // 开始播放。返回 false 表示 URL 非法、任务创建失败或防重播拦截。
    // on_finished 在播放任务结束时（自然播完/失败/被停止）回调一次。
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
    std::function<void()> on_finished_;
    mutable std::mutex mutex_;
    std::string last_url_;
    int64_t last_finished_ms_ = 0;
    std::string current_title_;
};

#endif  // _MUSIC_PLAYER_H_
