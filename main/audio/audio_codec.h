#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include <vector>
#include <string>
#include <functional>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
class AudioCodec {
public:
    AudioCodec();
    virtual ~AudioCodec();

    //控制接口
    virtual void SetOutputVolume(int volume);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);
    void Start();

    //数据读写接口
    bool InputData(std::vector<int16_t>& data);
    void OutputData(std::vector<int16_t>& data);

    //事件回调
    void OnOutputReady(std::function<bool()> callback);
    void OnInputReady(std::function<bool()> callback);

    //查询接口
    inline bool duplex() const {return duplex_;}
    inline int input_sample_rate() const {return input_sample_rate_;}
    inline int output_sample_rate() const {return output_sample_rate_;}
    inline int input_channels() const {return input_channels_;}
    inline int output_channels() const {return output_channels_;}
    inline int output_volume() const {return output_volume_;}

private:
    std::function<bool()> on_input_ready_;
    std::function<bool()> on_output_ready_;

    IRAM_ATTR static bool on_recv(i2s_chan_handle_t handle,i2s_event_data_t* event_data,void* user_ctx);
    IRAM_ATTR static bool on_sent(i2s_chan_handle_t handle,i2s_event_data_t* event_data,void* user_ctx);

protected:
    i2s_chan_handle_t tx_handle_=nullptr;
    i2s_chan_handle_t rx_handle_=nullptr;
    
    bool duplex_=false;
    bool input_reference_=false;
    bool input_enabled_=false;
    bool output_enabled_=false;
    int input_sample_rate_=16000;
    int output_sample_rate_=16000;
    int input_channels_=1;
    int output_channels_=1;
    int output_volume_=60;

    virtual int Read(int16_t* dest,int samples) = 0;
    virtual int Write(const int16_t* src,int samples) = 0;

};
#endif