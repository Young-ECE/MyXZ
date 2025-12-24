#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>
#include <math.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OnInputReady(std::function<bool()> callback) {
    on_input_ready_ = callback;
}

void AudioCodec::OnOutputReady(std::function<bool()> callback) {
    on_output_ready_ = callback;
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int duration = 30;
    int input_frame_size = input_sample_rate_ / 1000 * duration * input_channels_;

    data.resize(input_frame_size);
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        // ===== 添加调试代码 =====
        static int frame_count = 0;
        frame_count++;
        
        // 每隔33帧(约1秒)输出一次数据分析
        if (frame_count % 33 == 0) {
            // 计算RMS值
            float rms = 0.0f;
            int16_t max_val = INT16_MIN, min_val = INT16_MAX;
            
            for (int i = 0; i < samples; i++) {
                rms += (float)data[i] * data[i];
                if (data[i] > max_val) max_val = data[i];
                if (data[i] < min_val) min_val = data[i];
            }
            rms = sqrtf(rms / samples);
            
            ESP_LOGI("AudioDebug", "Frame %d: Samples=%d, RMS=%.1f, Range=[%d,%d]", 
                     frame_count, samples, rms, min_val, max_val);
            
            // 输出前10个原始采样值
            printf("Raw samples: ");
            for (int i = 0; i < 10 && i < samples; i++) {
                printf("%d ", data[i]);
            }
            printf("\n");
        }
        // ===== 调试代码结束 =====
        
        return true;
    }
    return false;
}

IRAM_ATTR bool AudioCodec::on_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
    auto audio_codec = (AudioCodec*)user_ctx;
    if (audio_codec->output_enabled_ && audio_codec->on_output_ready_) {
        return audio_codec->on_output_ready_();
    }
    return false;
}

IRAM_ATTR bool AudioCodec::on_recv(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
    auto audio_codec = (AudioCodec*)user_ctx;
    if (audio_codec->input_enabled_ && audio_codec->on_input_ready_) {
        return audio_codec->on_input_ready_();
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);

    // 注册音频数据回调
    i2s_event_callbacks_t rx_callbacks = {};
    rx_callbacks.on_recv = on_recv;
    i2s_channel_register_event_callback(rx_handle_, &rx_callbacks, this);

    i2s_event_callbacks_t tx_callbacks = {};
    tx_callbacks.on_sent = on_sent;
    i2s_channel_register_event_callback(tx_handle_, &tx_callbacks, this);

    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));

    EnableInput(true);
    EnableOutput(true);
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
