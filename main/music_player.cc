#include "music_player.h"

#include "audio/audio_service.h"
#include "audio_codec.h"
#include "board.h"
#include "system_info.h"
#include "audio/demuxer/ogg_demuxer.h"
#include "esp_audio_dec_default.h"
#include "esp_opus_dec.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#define TAG "MusicPlayer"

struct MusicPlayer::TaskArgs {
    MusicPlayer* player = nullptr;
    std::string url;
    std::string title;
    std::string artist;
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

    auto* args = new TaskArgs{this, url, title, artist};
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

void MusicPlayer::Run(TaskArgs* args) {
    const std::string& url = args->url;
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    auto network = board.GetNetwork();
    ESP_LOGI(TAG, "开始播放音乐（Opus 流）: %s - %s", args->title.c_str(), args->artist.c_str());

    // Opus 解码路径轻量（与 TTS 同款解码器），唤醒词引擎保持运行，
    // 用户可随时语音唤醒打断音乐（状态切换时由监听器停止播放）
    bool playback_failed = false;
    bool playback_finished = false;
    void* opus_dec = nullptr;
    std::vector<int16_t> pcm_buf(4800);  // 100ms@24k 单声道的余量
    size_t total_output_samples = 0;

    // 流参数完全由本机转码决定（24k 单声道 60ms 帧），解码器在连接 HTTP 之前
    // 用固定参数创建一次，避免运行期碎片化导致创建失败
    {
        esp_opus_dec_cfg_t cfg = {
            .sample_rate = 24000,
            .channel = ESP_AUDIO_MONO,
            .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
            .self_delimited = false,
        };
        auto ret = esp_opus_dec_open(&cfg, sizeof(cfg), &opus_dec);
        ESP_LOGI(TAG, "解码器创建: ret=%d 空闲=%u 最大块=%u", ret,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        if (ret != ESP_AUDIO_ERR_OK || opus_dec == nullptr) {
            ESP_LOGE(TAG, "Opus 解码器打开失败: %d", ret);
            playback_failed = true;
        }
    }

    OggDemuxer demuxer;
    demuxer.OnPacket([&](const uint8_t* data, int sample_rate, int frame_duration_ms, size_t size) {
        if (stop_requested_.load() || playback_failed || opus_dec == nullptr) {
            return;
        }

        esp_audio_dec_in_raw_t raw = {
            .buffer = const_cast<uint8_t*>(data),
            .len = static_cast<uint32_t>(size),
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t frame = {
            .buffer = reinterpret_cast<uint8_t*>(pcm_buf.data()),
            .len = static_cast<uint32_t>(pcm_buf.size() * sizeof(int16_t)),
            .decoded_size = 0,
        };
        esp_audio_dec_info_t dec_info = {};
        auto ret = esp_opus_dec_decode(opus_dec, &raw, &frame, &dec_info);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "Opus 解码失败: %d", ret);
            playback_failed = true;
            return;
        }
        if (frame.decoded_size > 0) {
            std::vector<int16_t> pcm(frame.decoded_size / sizeof(int16_t));
            memcpy(pcm.data(), frame.buffer, frame.decoded_size);
            if (!codec->output_enabled()) {
                codec->EnableOutput(true);
            }
            codec->OutputData(pcm);
            total_output_samples += pcm.size();
        }
    });

    constexpr int kReadBufSize = 4096;
    uint8_t* in_buf = static_cast<uint8_t*>(heap_caps_malloc(kReadBufSize, MALLOC_CAP_8BIT));
    auto http = network->CreateHttp(3);
    do {
        if (playback_failed) {
            break;  // 解码器都没建成，不必拉流
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
            // 喂给 OGG 解封装器，回调内完成 Opus 解码与播放
            size_t consumed = demuxer.Process(in_buf, read_bytes);
            (void)consumed;
        }
    } while (false);

    if (opus_dec != nullptr) {
        esp_opus_dec_close(opus_dec);
        opus_dec = nullptr;
    }
    if (in_buf != nullptr) {
        heap_caps_free(in_buf);
    }
    http->Close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_.store(false);
        last_url_ = url;
        last_finished_ms_ = esp_timer_get_time() / 1000;
        current_title_.clear();
    }

    if (stop_requested_.load()) {
        ESP_LOGI(TAG, "音乐播放已被停止");
    } else if (playback_failed) {
        ESP_LOGW(TAG, "音乐播放失败");
    } else if (playback_finished) {
        ESP_LOGI(TAG, "音乐播放结束，输出 %u samples", static_cast<unsigned>(total_output_samples));
    }
}
