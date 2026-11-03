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

// 有声书自动连播：向桥接请求下一段 URL（GET <base>/novel_next）。
// again=true 时请求重发当前段（网络抖动读坏后的自愈重试）。
// 返回空串表示不再连播（书读完/无进度/桥接未就绪）。
static std::string FetchNextSegmentUrl(NetworkInterface* network, const std::string& cur, bool again) {
    if (network == nullptr) {
        return "";
    }
    size_t s = cur.find("://");
    if (s == std::string::npos) {
        return "";
    }
    size_t p = cur.find('/', s + 3);
    std::string base = (p == std::string::npos) ? cur : cur.substr(0, p);
    auto http = network->CreateHttp(10);
    if (http == nullptr) {
        return "";
    }
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    if (!http->Open("GET", base + "/novel_next" + (again ? "?again=1" : ""))) {
        return "";
    }
    char buf[512];
    int total = 0;
    while (total < static_cast<int>(sizeof(buf)) - 1) {
        auto r = http->Read(buf + total, sizeof(buf) - 1 - total);
        if (!r || r.value() <= 0) {
            break;
        }
        total += r.value();
    }
    http->Close();
    std::string body(buf, total > 0 ? static_cast<size_t>(total) : 0);
    size_t a = body.find_first_not_of(" \t\r\n");
    size_t b = body.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) {
        return "";
    }
    body = body.substr(a, b - a + 1);
    if (body.rfind("http", 0) != 0) {
        return "";
    }
    return body;
}

// 播放管线：HTTP 拉 OGG 流 -> OggDemuxer 解出 Opus 包 -> 投递 AudioService
// 解码队列（复用开机已创建的官方解码器，与提示音/通知音完全同路径）。
// 本文件不自建任何解码器实例。
// 有声书片段（URL 含 /book_）自然播完后自动向桥接领取下一段连播；普通歌曲播完即停。
void MusicPlayer::Run(TaskArgs* args) {
    std::string url = args->url;
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

    // 唤醒词引擎保持常驻不释放：内存由生产者节流保障（队列 166 包→33 包，省 ~45K），
    // 销毁/重建 WakeNet 会拉低唤醒成功率（实测 2026-10-31）且丢掉语音打断能力，得不偿失。

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
        // 并长期占住——这是长播放内存耗尽 abort 重启的主因。把领先量压到 ~10 包
        // （0.6 秒音频，实测 33 包仍不够：官方 PCM 管线本身吃 ~35K），省出 ~50K；
        // 若生产落后实时（网络抖动）节流自动停拍，领先量自愈回补。
        const int64_t now_us = esp_timer_get_time();
        if (t_first_us == 0) {
            t_first_us = now_us;
        } else if (total_packets > 10) {
            const int64_t target_us =
                t_first_us + (static_cast<int64_t>(total_packets) - 10) * 60000;
            if (now_us < target_us) {
                vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>((target_us - now_us) / 1000) + 1));
            }
        }
    });

    constexpr int kReadBufSize = 2048;
    uint8_t* in_buf = static_cast<uint8_t*>(heap_caps_malloc(kReadBufSize, MALLOC_CAP_8BIT));
    int chain_retries = 0;  // 同一连播会话内"重发当前段"次数（防坏流死循环）
    for (;;) {
        playback_failed = false;
        playback_finished = false;
        const bool is_book = url.find("/book_") != std::string::npos;
        do {
            if (in_buf == nullptr) {
                ESP_LOGE(TAG, "内存不足");
                playback_failed = true;
                break;
            }
            auto http = network->CreateHttp(3);
            if (http == nullptr) {
                ESP_LOGE(TAG, "HTTP 客户端创建失败");
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

        // 有声书连播自愈：自然播完→取下一段；网络抖动读失败→重发当前段（最多 2 次）。
        // 只有"被停止"或自愈失败才断链，普通歌曲不受影响。
        if (!is_book || stop_requested_.load()) {
            break;
        }
        bool again = false;
        if (!playback_finished) {
            if (chain_retries >= 2) {
                ESP_LOGW(TAG, "有声书连播：重试次数用尽，停止连播");
                break;
            }
            chain_retries++;
            again = true;
            ESP_LOGW(TAG, "有声书连播：本段读取异常，重发当前段自愈（第%d次）", chain_retries);
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            chain_retries = 0;
        }
        std::string next = FetchNextSegmentUrl(network, url, again);
        if (next.empty()) {
            ESP_LOGI(TAG, "有声书连播结束（无下一段）");
            break;
        }
        ESP_LOGI(TAG, "有声书自动连播：%s", again ? "重发当前段" : "切换下一段");
        demuxer.Reset();
        url = std::move(next);
    }

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
