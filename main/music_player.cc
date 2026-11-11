#include "music_player.h"
#include <string_view>

#include "application.h"
#include "audio/audio_service.h"
#include "board.h"
#include "display/display.h"
#include "protocols/protocol.h"
#include "system_info.h"
#include "audio/demuxer/ogg_demuxer.h"

#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include <esp_log.h>
#include <esp_system.h>
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

// 轻字幕开关：2026-11-06 复盘定案——"秒崩"真凶是崩溃后立刻重试时堆已碎片化，
// 字幕显示路径的小额分配踩雷；根因(云端掐线重连风暴)已由 mqtt 重连闸门消除，
// 风暴不再→不再碎片化重试，字幕恢复（12字/句末断行的稳定形态）。
constexpr bool kSubtitlesEnabled = true;

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

// 字幕承载方式（2026-11-04 定稿）：桥接把字幕 JSON 写进 OGG 的 COMMENT 元数据、
// 与音频同一条连接下发——彻底消灭"第二条 HTTP 客户端交叠"这个秒崩根因。
// （曾经的 FetchHttpText 小文本助手因此退役。）

// 轻字幕条目扫描器：从 [{"t":ms,"s":"≤8字"},...] 顺序读出一条，零分配零拷贝
// （p 为游标，随播放进度单调前移；内存永远只有当前这一句）。
static bool NextCaptionEntry(const char*& p, const char* end, int& t_ms, char* out, size_t out_cap) {
    while (p < end && *p != '{') {
        if (*p == ']') {
            return false;
        }
        ++p;
    }
    if (p >= end) {
        return false;
    }
    ++p;  // '{'
    while (p < end && *p != ':') {
        ++p;
    }
    if (p >= end) {
        return false;
    }
    ++p;
    int t = 0;
    bool any = false;
    while (p < end && *p >= '0' && *p <= '9') {
        t = t * 10 + (*p - '0');
        ++p;
        any = true;
    }
    if (!any) {
        return false;
    }
    t_ms = t;
    // 越过 "s" 键到字符串值开头：键开引号→键闭引号→值开引号
    for (int q = 0; q < 3 && p < end; ++q) {
        while (p < end && *p != '"') {
            ++p;
        }
        if (p >= end) {
            return false;
        }
        ++p;
    }
    size_t n = 0;
    while (p < end) {
        char c = *p++;
        if (c == '"') {
            break;
        }
        if (c == '\\') {
            if (p >= end) {
                break;
            }
            char e = *p++;
            if (e == 'u') {
                for (int i = 0; i < 4 && p < end; ++i) {
                    ++p;
                }
                c = ' ';
            } else if (e == 'n') {
                c = ' ';
            } else {
                c = e;
            }
        }
        if (n + 1 < out_cap) {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    return n > 0;
}

// 播放管线：HTTP 拉 OGG 流 -> OggDemuxer 解出 Opus 包 -> 投递 AudioService
// 解码队列（复用开机已创建的官方解码器，与提示音/通知音完全同路径）。
// 本文件不自建任何解码器实例。
// 有声书片段（URL 含 /book_）自然播完后自动向桥接领取下一段连播；普通歌曲播完即停。
void MusicPlayer::Run(TaskArgs* args) {
    std::string url = args->url;
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto display = board.GetDisplay();
    // 轻字幕状态：指针游标 + 当前句（全部栈上 <100B，零堆分配——
    // 严禁在此申请大块：4.9K reserve 曾在 TTS 后碎片堆上直接 abort，2026-11-05 实测）
    const char* cap_p = nullptr;
    const char* cap_end = nullptr;
    int cap_next_t = -1;
    char cap_next_text[48] = {0};
    bool cap_init = false;
    bool cap_active = false;
    int64_t t_seg_us = 0;
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

    // 唤醒词引擎保持常驻：曾尝试"有声书会话释放引擎"（实测 2026-11-04），
    // 释放瞬间与音频通路/供电叠加导致 USB 掉线级联复位，风险大于收益，已回退。
    // 内存改由字幕瘦身（定长结构+4K上限）与更紧的节流（6 包）保障。

    // v43: demuxer 含 5200+2048+255B 固定缓冲(≈7.6KB)，必须堆分配——
    // bdd25ee 起误留栈上，8192B 任务栈进门即 Stack protection fault。
    auto demuxer_up = std::make_unique<OggDemuxer>();
    OggDemuxer& demuxer = *demuxer_up;
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
        } else if (total_packets > 6) {
            const int64_t target_us =
                t_first_us + (static_cast<int64_t>(total_packets) - 6) * 60000;
            if (now_us < target_us) {
                vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>((target_us - now_us) / 1000) + 1));
            }
        }
        // 轻字幕（交接说明方案）：OpusTags 注释包（SUBS=<时间轴>）随音频同流到达，
        // 指针按播放进度顺序 seek，内存只留当前一句；上屏 Schedule 回主循环执行。
        if (kSubtitlesEnabled) {
            if (!cap_init && demuxer.HasComment()) {
                cap_init = true;
                std::string_view raw = demuxer.comment();
                ESP_LOGI(TAG, "MP-C2 raw len=%u ready=%d", (unsigned)raw.size(),
                         (int)demuxer.HasComment());
                size_t s = (raw.size() <= 6000) ? raw.find("SUBS=")
                                                : std::string_view::npos;
                if (s != std::string_view::npos) {
                    cap_p = raw.data() + s + 5;
                    cap_end = raw.data() + raw.size();
                    cap_active = NextCaptionEntry(cap_p, cap_end, cap_next_t,
                                                  cap_next_text, sizeof(cap_next_text));
                    ESP_LOGI(TAG, "轻字幕就绪 %zu 字节 空闲堆=%u", raw.size(),
                             (unsigned)esp_get_free_heap_size());
                }
            }
            if (cap_active) {
                const int64_t tnow = esp_timer_get_time();
                if (t_seg_us == 0) {
                    t_seg_us = tnow;
                }
                int64_t pos_ms = (tnow - t_seg_us) / 1000 - 360;
                if (pos_ms < 0) {
                    pos_ms = 0;
                }
                while (cap_active && cap_next_t <= pos_ms) {
                    char shown[48];
                    // 有界拷贝，绝不越界读（cap_next_text 可能未以 NUL 结尾的极端情况）
                    strncpy(shown, cap_next_text, sizeof(shown) - 1);
                    shown[sizeof(shown) - 1] = '\0';
                    if (display != nullptr) {
                        Application::GetInstance().Schedule([shown]() {
                            auto d = Board::GetInstance().GetDisplay();
                            if (d != nullptr) {
                                d->SetChatMessage("system", shown);
                            }
                        });
                    }
                    cap_active = NextCaptionEntry(cap_p, cap_end, cap_next_t,
                                                  cap_next_text, sizeof(cap_next_text));
                }
            }
        }
    });

    ESP_LOGI(TAG, "MP-B0 Run entered urllen=%u", (unsigned)url.size());
    constexpr int kReadBufSize = 2048;
    uint8_t* in_buf = static_cast<uint8_t*>(heap_caps_malloc(kReadBufSize, MALLOC_CAP_8BIT));
    int chain_retries = 0;  // 同一连播会话内"重发当前段"次数（防坏流死循环）
    for (;;) {
        playback_failed = false;
        playback_finished = false;
        bool is_book = false;
        if (url.size() >= 8 && url.size() <= 200) {
            ESP_LOGI(TAG, "MP-C1 before url.find urllen=%u", (unsigned)url.size());
            is_book = url.find("/book_") != std::string::npos;
            ESP_LOGI(TAG, "MP-C1 after");
        } else {
            ESP_LOGE(TAG, "!! urllen ABNORMAL=%u", (unsigned)url.size());
        }
        cap_p = nullptr;
        cap_end = nullptr;
        cap_next_t = -1;
        cap_init = false;
        cap_active = false;
        t_seg_us = 0;
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

            unsigned loop_n = 0;
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
                if (((++loop_n) & 31) == 0) {
                    ESP_LOGI(TAG, "读循环#%u 空闲堆=%u", loop_n,
                             (unsigned)esp_get_free_heap_size());
                }
            }
            ESP_LOGI(TAG, "会话结束: finished=%d failed=%d stop=%d",
                     playback_finished ? 1 : 0, playback_failed ? 1 : 0,
                     stop_requested_.load() ? 1 : 0);
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
    // 注：新闻播完的"进聆听"已挪到 application.cc on_finished 内（唤醒恢复之后），
    // 避免与唤醒引擎恢复竞态（2026-11-08）。
}
