#ifndef _ESP32S3_AUDIO_CODEC_H
#define _ESP32S3_AUDIO_CODEC_H

#include "audio_codec.h"
#include <driver/gpio.h>

// ESP32-S3音频编解码器（使用I2S标准模式，Simplex单工模式，独立通道）
// 麦克风和扬声器使用独立的I2S通道和GPIO引脚
class Esp32S3AudioCodec : public AudioCodec {
public:
    // 构造函数：Simplex模式配置I2S GPIO引脚
    // 参数说明：
    // - input_sample_rate: 输入采样率（通常16000）
    // - output_sample_rate: 输出采样率（通常16000或24000）
    // - spk_bclk: 扬声器I2S位时钟GPIO
    // - spk_ws: 扬声器I2S字选择（LRCLK）GPIO
    // - spk_dout: 扬声器I2S数据输出GPIO（连接MAX98357A等）
    // - mic_sck: 麦克风I2S位时钟GPIO
    // - mic_ws: 麦克风I2S字选择GPIO
    // - mic_din: 麦克风I2S数据输入GPIO（连接INMP441等）
    Esp32S3AudioCodec(int input_sample_rate, 
                       int output_sample_rate,
                       gpio_num_t spk_bclk, 
                       gpio_num_t spk_ws, 
                       gpio_num_t spk_dout, 
                       gpio_num_t mic_sck, 
                       gpio_num_t mic_ws, 
                       gpio_num_t mic_din);
    
    virtual ~Esp32S3AudioCodec();

private:
    virtual int Write(const int16_t* data, int samples) override;
    virtual int Read(int16_t* dest, int samples) override;
};

#endif // _ESP32S3_AUDIO_CODEC_H