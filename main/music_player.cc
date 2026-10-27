#include "music_player.h"

#include "audio/audio_service.h"
#include "audio_codec.h"
#include "board.h"
#include "system_info.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_ae_rate_cvt.h"

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
    on_finished_ = std::move(on_finished);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_title_ = title;
    }
    stop_requested_.store(false);
    playing_.store(true);

    auto* args = new TaskArgs{this, url, title, artist};
    TaskHandle_t handle = nullptr;
    if (xTaskCreate(&MusicPlayer::TaskEntry, "music_playback", 12288, args, 3, &handle) != pdPASS) {
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
    // FreeRTOS 任务函数禁止 return，必须自行删除（否则返回到空指针触发 Guru Meditation）
    vTaskDelete(nullptr);
}

void MusicPlayer::Run(TaskArgs* args) {
    const std::string& url = args->url;
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    auto network = board.GetNetwork();
    ESP_LOGI(TAG, "开始播放音乐: %s - %s", args->title.c_str(), args->artist.c_str());

    if (audio_service_ != nullptr) {
        audio_service_->SetExternalPlaybackActive(true);
    }
    // HTTP 流不经过音频通道，需手动关闭 WiFi 省电，避免下载卡顿
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    // 等主循环把 idle 状态的唤醒词引擎拉起后，再关掉并释放它：
    // C3 无 PSRAM，音乐 TLS 连接需要 ~17KB 连续内存，必须让 AFE 腾位置
    vTaskDelay(pdMS_TO_TICKS(400));
    if (audio_service_ != nullptr) {
        audio_service_->EnableWakeWordDetection(false);
        audio_service_->ReleaseWakeWordResources();
    }

    constexpr int kReadBufSize = 2048;
    constexpr int kMaxZeroReads = 30;  // 连续空读约 3 秒视为流结束，容忍网络抖动
    int out_buf_size = 8192;
    bool decoder_registered = false;
    bool playback_failed = false;
    bool playback_finished = false;
    esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    uint8_t* in_buf = nullptr;
    uint8_t* out_buf = nullptr;
    bool info_ready = false;
    int stream_sample_rate = codec->output_sample_rate();
    int stream_channels = std::max(1, codec->output_channels());
    int zero_read_count = 0;
    size_t total_output_samples = 0;

    auto http = network->CreateHttp(3);
    do {
        http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
        bool opened_ok = false;
        for (int attempt = 0; attempt < 2 && !stop_requested_.load(); ++attempt) {
            if (auto opened = http->Open("GET", url); opened) {
                opened_ok = true;
                break;
            }
            ESP_LOGW(TAG, "打开音频链接失败，%d ms 后重试", 800);
            vTaskDelay(pdMS_TO_TICKS(800));
        }
        if (!opened_ok) {
            ESP_LOGE(TAG, "无法打开音频链接: %s", url.c_str());
            playback_failed = true;
            break;
        }
        if (http->GetStatusCode() != 200) {
            ESP_LOGE(TAG, "音频链接返回状态码: %d", http->GetStatusCode());
            playback_failed = true;
            break;
        }
        ret = esp_audio_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "注册默认解码器失败: %d", ret);
            playback_failed = true;
            break;
        }
        ret = esp_audio_simple_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "注册简易解码器失败: %d", ret);
            playback_failed = true;
            break;
        }
        decoder_registered = true;
        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = nullptr,
            .cfg_size = 0,
            .use_frame_dec = false,
        };
        ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
        if (ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
            ESP_LOGE(TAG, "打开 MP3 解码器失败: %d", ret);
            playback_failed = true;
            break;
        }
        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }
        in_buf = static_cast<uint8_t*>(heap_caps_malloc(kReadBufSize, MALLOC_CAP_8BIT));
        out_buf = static_cast<uint8_t*>(heap_caps_malloc(out_buf_size, MALLOC_CAP_8BIT));
        if (in_buf == nullptr || out_buf == nullptr) {
            ESP_LOGE(TAG, "内存不足，无法播放音乐");
            playback_failed = true;
            break;
        }

        while (!stop_requested_.load()) {
            auto read_res = http->Read(reinterpret_cast<char*>(in_buf), kReadBufSize);
            if (!read_res) {
                ESP_LOGW(TAG, "读取音频流失败: %s", read_res.error().ToString().c_str());
                playback_failed = true;
                break;
            }
            int read_bytes = read_res.value();
            if (read_bytes == 0) {
                zero_read_count++;
                if (zero_read_count >= kMaxZeroReads) {
                    ESP_LOGI(TAG, "音频流读取结束（连续空读 %d 次）", zero_read_count);
                    playback_finished = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            zero_read_count = 0;

            esp_audio_simple_dec_raw_t raw = {
                .buffer = in_buf,
                .len = static_cast<uint32_t>(read_bytes),
                .eos = read_bytes < kReadBufSize,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            while (raw.len > 0 && !stop_requested_.load()) {
                esp_audio_simple_dec_out_t out = {
                    .buffer = out_buf,
                    .len = static_cast<uint32_t>(out_buf_size),
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                ret = esp_audio_simple_dec_process(decoder, &raw, &out);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    auto* new_buf = static_cast<uint8_t*>(
                        heap_caps_realloc(out_buf, out.needed_size, MALLOC_CAP_8BIT));
                    if (new_buf == nullptr) {
                        ESP_LOGE(TAG, "扩容解码输出缓冲区失败");
                        playback_failed = true;
                        break;
                    }
                    out_buf = new_buf;
                    out_buf_size = static_cast<int>(out.needed_size);
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(TAG, "MP3 解码失败 ret=%d", ret);
                    playback_failed = true;
                    break;
                }
                if (raw.consumed == 0 && out.decoded_size == 0) {
                    // 解码无进展，跳出避免死循环（数据损坏等场景）
                    ESP_LOGW(TAG, "解码无进展，提前结束");
                    break;
                }
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;

                if (out.decoded_size == 0) {
                    continue;
                }
                if (!info_ready) {
                    esp_audio_simple_dec_info_t dec_info = {};
                    if (esp_audio_simple_dec_get_info(decoder, &dec_info) == ESP_AUDIO_ERR_OK) {
                        stream_sample_rate = static_cast<int>(dec_info.sample_rate);
                        stream_channels = std::max(1, static_cast<int>(dec_info.channel));
                        info_ready = true;
                        ESP_LOGI(TAG, "MP3 流信息: sample_rate=%d channel=%d",
                                 stream_sample_rate, stream_channels);
                    }
                }

                std::vector<int16_t> pcm(out.decoded_size / sizeof(int16_t));
                memcpy(pcm.data(), out.buffer, out.decoded_size);

                // 声道统一到 codec 输出声道
                if (stream_channels == 2 && codec->output_channels() == 1) {
                    std::vector<int16_t> mono(pcm.size() / 2);
                    for (size_t i = 0, j = 0; i < mono.size(); ++i, j += 2) {
                        mono[i] = static_cast<int16_t>(
                            (static_cast<int32_t>(pcm[j]) + static_cast<int32_t>(pcm[j + 1])) / 2);
                    }
                    pcm = std::move(mono);
                } else if (stream_channels == 1 && codec->output_channels() == 2) {
                    std::vector<int16_t> stereo(pcm.size() * 2);
                    for (size_t i = 0; i < pcm.size(); ++i) {
                        stereo[i * 2] = pcm[i];
                        stereo[i * 2 + 1] = pcm[i];
                    }
                    pcm = std::move(stereo);
                }

                // 采样率不一致时重采样，避免变速/变调
                if (stream_sample_rate != codec->output_sample_rate()) {
                    if (resampler == nullptr) {
                        esp_ae_rate_cvt_cfg_t cfg = {
                            .src_rate = static_cast<uint32_t>(stream_sample_rate),
                            .dest_rate = static_cast<uint32_t>(codec->output_sample_rate()),
                            .channel = static_cast<uint8_t>(codec->output_channels()),
                            .bits_per_sample = ESP_AUDIO_BIT16,
                            .complexity = 2,
                            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                        };
                        auto cvt_ret = esp_ae_rate_cvt_open(&cfg, &resampler);
                        if (resampler == nullptr) {
                            ESP_LOGE(TAG, "创建重采样器失败: %d", cvt_ret);
                            playback_failed = true;
                            break;
                        }
                    }
                    uint32_t in_samples = static_cast<uint32_t>(pcm.size() / codec->output_channels());
                    uint32_t out_samples = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, in_samples, &out_samples);
                    std::vector<int16_t> resampled(out_samples * codec->output_channels());
                    uint32_t actual_out = out_samples;
                    esp_ae_rate_cvt_process(resampler,
                                            reinterpret_cast<esp_ae_sample_t>(pcm.data()), in_samples,
                                            reinterpret_cast<esp_ae_sample_t>(resampled.data()),
                                            &actual_out);
                    resampled.resize(actual_out * codec->output_channels());
                    pcm = std::move(resampled);
                }

                if (!pcm.empty()) {
                    total_output_samples += pcm.size();
                    // 音乐播放绕过 AudioService，需要手动保持功放开启
                    if (!codec->output_enabled()) {
                        codec->EnableOutput(true);
                    }
                    codec->OutputData(pcm);
                }
            }
            if (playback_failed || stop_requested_.load()) {
                break;
            }
        }
    } while (false);

    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
    }
    if (in_buf != nullptr) {
        heap_caps_free(in_buf);
    }
    if (out_buf != nullptr) {
        heap_caps_free(out_buf);
    }
    if (decoder != nullptr) {
        esp_audio_simple_dec_close(decoder);
    }
    if (decoder_registered) {
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
    }
    http->Close();
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
        ESP_LOGI(TAG, "音乐播放已被停止");
    } else if (playback_failed) {
        ESP_LOGW(TAG, "音乐播放失败");
    } else if (playback_finished) {
        ESP_LOGI(TAG, "音乐播放结束，输出 %u samples", static_cast<unsigned>(total_output_samples));
    }

    if (on_finished_) {
        auto cb = std::move(on_finished_);
        on_finished_ = nullptr;
        cb();
    }
}
