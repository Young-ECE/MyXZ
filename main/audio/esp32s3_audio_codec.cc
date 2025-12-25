#include "esp32s3_audio_codec.h"
#include <esp_log.h>
#include <cmath>
#include <cstring>

#define TAG "Esp32S3AudioCodec"

Esp32S3AudioCodec::Esp32S3AudioCodec(int input_sample_rate, 
                                      int output_sample_rate,
                                      gpio_num_t spk_bclk, 
                                      gpio_num_t spk_ws, 
                                      gpio_num_t spk_dout, 
                                      gpio_num_t mic_sck, 
                                      gpio_num_t mic_ws, 
                                      gpio_num_t mic_din) {
    duplex_ = false;  // Simplex单工模式
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    // ========== 创建扬声器输出通道（I2S_NUM_0） ==========
    i2s_chan_config_t chan_cfg = {
        .id = (i2s_port_t)0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));

    // ========== 创建麦克风输入通道（I2S_NUM_1） ==========
    chan_cfg.id = (i2s_port_t)1;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    
    ESP_LOGI(TAG, "ESP32-S3 AudioCodec (Simplex) initialized");
    ESP_LOGI(TAG, "  Input:  %dHz (MIC: SCK=%d, WS=%d, DIN=%d)", 
             input_sample_rate_, mic_sck, mic_ws, mic_din);
    ESP_LOGI(TAG, "  Output: %dHz (SPK: BCLK=%d, WS=%d, DOUT=%d)", 
             output_sample_rate_, spk_bclk, spk_ws, spk_dout);
}

Esp32S3AudioCodec::~Esp32S3AudioCodec() {
    if (rx_handle_ != nullptr) {
        i2s_channel_disable(rx_handle_);
    }
    if (tx_handle_ != nullptr) {
        i2s_channel_disable(tx_handle_);
    }
}

int Esp32S3AudioCodec::Write(const int16_t* data, int samples) {
    std::vector<int32_t> buffer(samples);

    // 音量控制：output_volume_范围0-100，转换为0-65536的因子
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;
    
    for (int i = 0; i < samples; i++) {
        int64_t temp = int64_t(data[i]) * volume_factor;
        if (temp > INT32_MAX) {
            buffer[i] = INT32_MAX;
        } else if (temp < INT32_MIN) {
            buffer[i] = INT32_MIN;
        } else {
            buffer[i] = static_cast<int32_t>(temp);
        }
    }

    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(), 
                                       samples * sizeof(int32_t), 
                                       &bytes_written, portMAX_DELAY));
    return bytes_written / sizeof(int32_t);
}

int Esp32S3AudioCodec::Read(int16_t* dest, int samples) {
    size_t bytes_read;
    std::vector<int32_t> bit32_buffer(samples);
    
    if (i2s_channel_read(rx_handle_, bit32_buffer.data(), 
                         samples * sizeof(int32_t), 
                         &bytes_read, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "I2S read failed");
        return 0;
    }

    samples = bytes_read / sizeof(int32_t);
    
    // 对于24位I2S麦克风（如INMP441），数据在32位容器中的布局：
    // 24位有效数据通常在高24位（bit[31:8]），低8位为0
    // 需要右移8位将24位数据转换为16位
    // 注意：原项目使用>>12可能是针对特定格式，但我们使用>>8更适合24位麦克风
    
    // 添加调试：每100帧打印一次原始数据（用于分析24位麦克风数据）
    static int debug_frame_count = 0;
    debug_frame_count++;
    if (debug_frame_count % 100 == 0 && samples > 0) {
        ESP_LOGI(TAG, "=== Raw I2S Data Debug (Frame %d) ===", debug_frame_count);
        ESP_LOGI(TAG, "Samples read: %d", samples);
        // 打印前5个原始32位值
        for (int i = 0; i < 5 && i < samples; i++) {
            int32_t raw = bit32_buffer[i];
            int32_t val_8 = raw >> 8;   // 24位转16位（正确方式）
            int32_t val_12 = raw >> 12; // 原项目的做法
            ESP_LOGI(TAG, "Raw[%d]: 0x%08lx (%ld) -> >>8:%d >>12:%d", 
                     i, (unsigned long)raw, (long)raw, (int)val_8, (int)val_12);
        }
    }
    
    // 将32位数据转换为16位（24位麦克风：右移8位）
    for (int i = 0; i < samples; i++) {
        // 先右移8位（24位到16位），然后再限制范围
        int32_t value = bit32_buffer[i] >> 8;
        dest[i] = (value > INT16_MAX) ? INT16_MAX : 
                  (value < INT16_MIN) ? INT16_MIN : 
                  (int16_t)value;
    }
    
    return samples;
}