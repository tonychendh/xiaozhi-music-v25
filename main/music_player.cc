#include "music_player.h"

#include "audio/audio_service.h"
#include "board.h"
#include "protocols/protocol.h"
#include "system_info.h"
#include "audio/demuxer/ogg_demuxer.h"

#include <cstring>
#include <memory>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#define TAG "MusicPlayer"

struct MusicPlayer::TaskArgs {
    MusicPlayer* player = nullptr;
    std::string url;
    std::string title;
    std::string artist;
    std::function<void()> on_finished;
};

MusicPlayer::~MusicPlayer() {
    Stop();
}

std::string MusicPlayer::current_title() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_title_;
}

bool MusicPlayer::Start(const std::string& url, const std::string& title, const std::string& artist,
                         AudioService& audio_service, std::function<void()> on_finished) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        ESP_LOGW(TAG, "无效的音乐链接: %s", url.c_str());
        return false;
    }
    {
        // 防重播：同一 URL 播完后 15 秒内不允许立即重播，
        // 防止大模型“播完 -> 回答 -> 又调一次 play_url”的死循环
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_url_.empty() && last_url_ == url && last_finished_ms_ > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - last_finished_ms_ < 15000) {
                ESP_LOGW(TAG, "防重播：同一链接 %lld ms 前刚播完，跳过",
                         static_cast<long long>(now_ms - last_finished_ms_));
                return false;
            }
        }
    }

    Stop();
    audio_service_ = &audio_service;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_title_ = title;
    }
    stop_requested_.store(false);
    playing_.store(true);

    auto* args = new TaskArgs{this, url, title, artist, std::move(on_finished)};
    TaskHandle_t handle = nullptr;
    if (xTaskCreate(&MusicPlayer::TaskEntry, "music_playback", 8192, args, 3, &handle) != pdPASS) {
        delete args;
        playing_.store(false);
        audio_service_ = nullptr;
        ESP_LOGE(TAG, "创建播放任务失败（内存不足）");
        return false;
    }
    task_handle_.store(handle);
    return true;
}

void MusicPlayer::Stop() {
    stop_requested_.store(true);
    if (xTaskGetCurrentTaskHandle() == task_handle_.load()) {
        // 播放任务自身上下文：只置停止位，不等待
        return;
    }
    while (task_handle_.load() != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    stop_requested_.store(false);
}

void MusicPlayer::TaskEntry(void* arg) {
    std::unique_ptr<TaskArgs> args(static_cast<TaskArgs*>(arg));
    auto* self = args->player;
    self->Run(args.get());
    self->task_handle_.store(nullptr);
    // FreeRTOS 任务函数禁止 return，必须自行删除
    vTaskDelete(nullptr);
}

// 播放管线：HTTP 拉 OGG 流 -> OggDemuxer 解出 Opus 包 -> 投递 AudioService
// 解码队列（复用开机已创建的官方解码器，与提示音/通知音完全同路径）。
// 本文件不自建任何解码器实例。
void MusicPlayer::Run(TaskArgs* args) {
    const std::string& url = args->url;
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    ESP_LOGI(TAG, "开始播放音乐（解码队列模式）: %s - %s", args->title.c_str(), args->artist.c_str());

    bool playback_failed = false;
    bool playback_finished = false;
    size_t total_packets = 0;
    int64_t t_first_us = 0;  // 首包投递时刻，生产者节流基准

    // 保持功放电源常开，避免长音频期间被省电逻辑关闭
    if (audio_service_ != nullptr) {
        audio_service_->SetExternalPlaybackActive(true);
    }
    // HTTP 拉流期间关闭 WiFi 省电，避免下载卡顿
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    // C3 无 PSRAM，空闲内部 RAM 仅 ~57K，而播放管线（HTTP+解封装+解码/播放队列+任务栈）
    // 稳态要吃 ~48K，叠加报幕 TTS / MQTT TLS 重连即内存耗尽（实测 alloc failed + abort 复位）。
    // 照抄官方通知音路径（application.cc StartNotification）：播放期间临时释放唤醒词引擎
    // 腾出 ~20K+；播放结束经 on_finished 回调恢复（Application 已负责 Schedule 回主循环）。
    // 代价：音乐播放期间不支持语音唤醒打断，BOOT 键停止仍可用。
    if (audio_service_ != nullptr) {
        audio_service_->EnableVoiceProcessing(false);
        audio_service_->EnableWakeWordDetection(false);
        audio_service_->ReleaseWakeWordResources();
    }

    OggDemuxer demuxer;
    demuxer.OnPacket([&](const uint8_t* data, int sample_rate, int frame_duration_ms, size_t size) {
        (void)sample_rate;
        (void)frame_duration_ms;
        if (stop_requested_.load() || playback_failed || audio_service_ == nullptr) {
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        // 流由本机转码生成，固定 24k 单声道 60ms，与设备解码器配置一致
        packet->sample_rate = 24000;
        packet->frame_duration = 60;
        packet->payload.assign(data, data + size);
        if (!audio_service_->PushPacketToDecodeQueue(std::move(packet), true)) {
            ESP_LOGW(TAG, "解码队列投递失败（队列关闭或超时）");
            playback_failed = true;
            return;
        }
        total_packets++;
        // 生产者节流：官方解码队列深 166 包（~55K 堆内存），局域网速度会瞬间灌满
        // 并长期占住——这是长播放（15~20 分钟后）MQTT/TLS 重连等内存大户拿不到
        // 内存、最终 abort 重启的主因。把领先量压到 ~33 包（2 秒音频），省出 ~45K；
        // 若生产落后实时（网络抖动）节流自动停拍，领先量自愈回补。
        const int64_t now_us = esp_timer_get_time();
        if (t_first_us == 0) {
            t_first_us = now_us;
        } else if (total_packets > 33) {
            const int64_t target_us =
                t_first_us + (static_cast<int64_t>(total_packets) - 33) * 60000;
            if (now_us < target_us) {
                vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>((target_us - now_us) / 1000) + 1));
            }
        }
    });

    constexpr int kReadBufSize = 4096;
    uint8_t* in_buf = static_cast<uint8_t*>(heap_caps_malloc(kReadBufSize, MALLOC_CAP_8BIT));
    auto http = network->CreateHttp(3);
    do {
        if (in_buf == nullptr) {
            ESP_LOGE(TAG, "内存不足");
            playback_failed = true;
            break;
        }
        http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
        bool opened_ok = false;
        for (int attempt = 0; attempt < 2 && !stop_requested_.load(); ++attempt) {
            if (auto opened = http->Open("GET", url); opened) {
                opened_ok = true;
                break;
            }
            ESP_LOGW(TAG, "打开音乐流失败，800ms 后重试");
            vTaskDelay(pdMS_TO_TICKS(800));
        }
        if (!opened_ok) {
            ESP_LOGE(TAG, "无法打开音乐流: %s", url.c_str());
            playback_failed = true;
            break;
        }

        while (!stop_requested_.load() && !playback_failed) {
            auto read_res = http->Read(reinterpret_cast<char*>(in_buf), kReadBufSize);
            if (!read_res) {
                ESP_LOGW(TAG, "读取音乐流失败: %s", read_res.error().ToString().c_str());
                playback_failed = true;
                break;
            }
            int read_bytes = read_res.value();
            if (read_bytes == 0) {
                ESP_LOGI(TAG, "音乐流读取结束");
                playback_finished = true;
                break;
            }
            demuxer.Process(in_buf, read_bytes);
        }
    } while (false);

    http->Close();
    if (in_buf != nullptr) {
        heap_caps_free(in_buf);
    }
    if (audio_service_ != nullptr) {
        audio_service_->SetExternalPlaybackActive(false);
    }
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_.store(false);
        last_url_ = url;
        last_finished_ms_ = esp_timer_get_time() / 1000;
        current_title_.clear();
    }

    if (stop_requested_.load()) {
        ESP_LOGI(TAG, "音乐播放已被停止（已投递 %u 包）", static_cast<unsigned>(total_packets));
    } else if (playback_failed) {
        ESP_LOGW(TAG, "音乐播放失败");
    } else if (playback_finished) {
        ESP_LOGI(TAG, "音乐播放结束，共投递 %u 包", static_cast<unsigned>(total_packets));
    }

    // 唤醒词引擎恢复的唯一收口点：正常播完/失败/被停止都会走到这里。
    // 回调内部 Application::Schedule 回主循环判 idle 后 EnableWakeWordDetection(true)，
    // 该路径会自动 RestoreWakeWordResources 重建 WakeNet。
    if (args->on_finished) {
        args->on_finished();
    }
}
