#ifndef MELONDS_OBOECALLBACK_H
#define MELONDS_OBOECALLBACK_H

#include <oboe/Oboe.h>
#include <fstream>
#include "MelonInstance.h"

class OboeCallback : public oboe::AudioStreamCallback {
private:
    int _volume;
    void (*onErrorCallback)(void);
    std::ostream* _recordingStream;

    // 加速期 skew 控制器状态（设计 D2），仅构造函数与音频回调线程访问
    bool lastFastForwardEnabled;        // 上一回调周期的加速标志快照（转换检测）
    float lastMultiplier;               // 上一回调周期的倍速快照（加速中倍速热更新检测）
    double emaMeasuredRatio;            // 实测比率（fps/基准帧率）的 EMA 平滑值——加速期 skew 的前馈主部
                                        // （稳态 skew = 本值 × 水位反馈再总钳，见 onAudioReady；转换回调时等于种子）
    bool awaitingFirstMeasurement;      // FF-on 后等待首个新 fps 测量（到达时直接快照 EMA）
    float fpsAtTransition;              // 置待首测时刻的 fps 快照（值变化 = 新测量到达）
    int awaitingFirstMeasurementCycles; // 待首测已等待的回调数（超时兜底快照）
    bool pendingBufferRebuild;          // 本回调读取输出后需执行的缓冲余量重建（FF-on/档位转换）
    bool audioMuted;                    // D7 迟滞静音态：超过静音线置真、低于恢复线（0.8×阈值）复位；
                                        // 流重建（新构造）与加速标志翻转为关时复位为未静音
    double appliedSkew;                 // 上一回调实际应用的 skew（变化率限制的基准；转换/常速分支直接重设）
    int feedbackFreezeCycles;           // 安全阀 Trim 后的反馈冻结计数——Trim 把水位阶跃回半满，
                                        // 立即反馈会读到假低水位而撤销修正、随超产回贴形成锯齿（真机实测缺陷）
    int valveLogCooldown;               // 安全阀日志冷却计数（实时线程高频日志是纯噪声，触发段降频）

public:
    std::weak_ptr<MelonDSAndroid::MelonInstance> activeInstance;

    OboeCallback(int volume, void (*onErrorCallback)(void)) : OboeCallback(volume, onErrorCallback, nullptr) { };
    OboeCallback(int volume, void (*onErrorCallback)(void), std::ostream* recordingStream);
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *stream, void *audioData, int32_t numFrames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result result) override;
};


#endif //MELONDS_OBOECALLBACK_H
