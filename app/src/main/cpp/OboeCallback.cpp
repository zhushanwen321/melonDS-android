#include "OboeCallback.h"
#include "types.h"
#include "Platform.h"
#include "MelonLog.h"

#include <algorithm>
#include <atomic>
#include <cmath>

using namespace melonDS;

#define INTERNAL_FRAME_RATE 59.8260982880808f

// 定义于 MelonDSAndroidJNI.cpp：模拟/JNI 线程写、音频回调线程读（设计 D5）
extern std::atomic<bool> isFastForwardEnabled;
extern std::atomic<float> fps;
extern std::atomic<float> fastForwardSpeedMultiplier;
extern std::atomic<float> fastForwardAudioMuteThreshold;

// 常速基准比率：60fps 目标对 NDS 实际帧率的换算（≈1.0029）
static constexpr double kBaseSkewRatio = 60.0 / INTERNAL_FRAME_RATE;

// SPU 环形缓冲为 2048 立体声帧（≈43ms），此处为其半容量
static constexpr int kAudioOutputHalfCapacity = 1024;

// 全容量 = 2 × 半容量（安全阀阈值与低水位观察线的共同推导基准）
static constexpr int kAudioOutputFullCapacity = 2 * kAudioOutputHalfCapacity;

// 高水位安全阀阈值（设计 D4）：85% 容量 ≈ 1740 帧。触发后 Trim 把水位放回半满自然退出，
// 无冷却期、无次数上限；正常常速（速率匹配、水位半满附近）永不触及
static constexpr int kAudioOutputHighWaterThreshold = (int) (kAudioOutputFullCapacity * 0.85);

// 待首测的回调数兜底上限：测量窗口可能在转换被回调观察到之前已走完（调度延迟），
// 此时 fps 值不再变化、快照比较永不命中，靠超时直接快照当前值（设计 D2 规则 3）
static constexpr int kMaxCyclesAwaitingFirstMeasurement = 8;

// FF-on/档位转换的 skew 种子（设计 D2 规则 4/5）：固定档 = 倍速 × 基准（设备撑得住目标帧率时即
// 精确值）；不限速档保守取 1.3 × 基准——种子必须低于真实倍速（设置项最小定倍速 1.5），让缓冲
// 从空回填而非超产顶样
static double seedSkewForMultiplier(float multiplier)
{
    double speedFactor = multiplier > 0 ? (double) multiplier : 1.3;
    return speedFactor * kBaseSkewRatio;
}

// 实测比率 = 实测帧率 / NDS 实际帧率；fps 尚无测量（0）时按常速基准处理（设计 §7.4）
static double measuredRatioFromFps(float measuredFps)
{
    return (measuredFps > 0.0f ? (double) measuredFps : 60.0) / INTERNAL_FRAME_RATE;
}

// 水位反馈修正因子（设计 D2 规则 6）：fillErr = (填充水位 − 半容量)/半容量 ∈ [−1, 1]，
// 连续增益 K(e) = 0.05 + 0.25|e|——分段增益在区间边界跳变会产生抖振（设计 D2 不采用 (vi)），
// 乘积钳 ±2%：反馈直接调制产样密度，而产样密度就是音调（音调 = 真实倍速 ÷ skew），
// 修正幅度必须压在听觉不可辨的范围内（真机实测 ±10% 会形成可听的音调锯齿）
static double fillFeedbackFactor(int fillLevel)
{
    double fillErr = (fillLevel - kAudioOutputHalfCapacity) / (double) kAudioOutputHalfCapacity;
    return std::clamp(1.0 + (0.05 + 0.25 * std::fabs(fillErr)) * fillErr, 0.98, 1.02);
}

// skew 变化率限制（每回调 ±1%）：skew 经 blip 换算直接决定波形密度，动态跳变既产生
// 帧内时序断裂（blip_set_rates 与模拟线程帧内 blip_add_delta 竞争），也表现为音调阶跃；
// 平滑到目标而非一步跳达。回调周期 4-10ms 下每秒可移动 2.7 倍范围，远快于测量收敛
static double slewLimitSkew(double target, double previous)
{
    return std::clamp(target, previous * 0.99, previous * 1.01);
}

OboeCallback::OboeCallback(int volume, void (*onErrorCallback)(void), std::ostream* recordingStream) : _volume(volume), onErrorCallback(onErrorCallback), _recordingStream(recordingStream) {
    // 控制器状态按出生态初始化（设计 D2 规则 8）：音频流重建会新造本对象，此时加速可能早已开启、
    // 标志翻转永不发生——构造时已处于加速态 = 按一次 FF-on 转换初始化
    bool fastForward = isFastForwardEnabled.load(std::memory_order_acquire);
    float multiplier = fastForwardSpeedMultiplier.load(std::memory_order_relaxed);

    lastFastForwardEnabled = fastForward;
    lastMultiplier = multiplier;
    fpsAtTransition = fps.load(std::memory_order_acquire);
    awaitingFirstMeasurementCycles = 0;
    pendingBufferRebuild = false;
    // 迟滞静音态按出生态复位（设计 D2 规则 8 生命周期表）；EMA 初值已含 fps 快照成分，
    // 首个回调的出生态首判定即能正确重进静音/发声，无「种子先有声、首测再静音」的二次阶跃
    audioMuted = false;

    if (fastForward)
    {
        // EMA 初始值 = max(种子, 当前 fps 快照/基准)（设计 D2 规则 8）：fps 快照是最近一个
        // 测量窗口的实测，流重建前已实测超线时出生首判定即有据可依；快照尚无测量（0）时
        // measuredRatioFromFps 按常速基准处理，max 退化为种子本身
        emaMeasuredRatio = std::max(seedSkewForMultiplier(multiplier),
                                    measuredRatioFromFps(fps.load(std::memory_order_acquire)));
        awaitingFirstMeasurement = true;
        appliedSkew = emaMeasuredRatio;
        // 缓冲余量重建推迟到首个回调、在读取输出之后执行（动作次序见 onAudioReady）——
        // 构造时实例可能尚未绑定，也没有本回调的读取需要保护
        pendingBufferRebuild = true;
    }
    else
    {
        emaMeasuredRatio = kBaseSkewRatio;
        awaitingFirstMeasurement = false;
        appliedSkew = kBaseSkewRatio;
    }
    feedbackFreezeCycles = 0;
    valveLogCooldown = 0;
}

oboe::DataCallbackResult
OboeCallback::onAudioReady(oboe::AudioStream *stream, void *audioData, int32_t numFrames) {
    auto currentInstance = activeInstance.lock();

    if (!currentInstance)
    {
        memset(audioData, 0, numFrames * sizeof(u16) * 2);
        return oboe::DataCallbackResult::Continue;
    }

    bool fastForward = isFastForwardEnabled.load(std::memory_order_acquire);
    float multiplier = fastForwardSpeedMultiplier.load(std::memory_order_relaxed);

    // 填充水位：安全阀与水位反馈共用这一次读取（设计 §7.5）；下方缓冲重建路径另有一次独立
    // 读取，取的是本回调消费后的新鲜水位，两处目的不同不共用
    int fillLevel = currentInstance->getAudioOutputSize();

    // 高水位安全阀（设计 D4，音频活跃期间不限加速状态）：贴满态一次性 Trim 回半满，
    // 覆盖常速贴满（如短于一个回调周期的加速脉冲遗留）与流重建积压。无需再比半容量：
    // 85% 触发阈值天然高于半容量——水位 ≤ 半容量时该条件永不成立，Trim 反而会把
    // 读指针向回拨、重播已消费样本。
    // 三项修复（真机实测缺陷）：①日志冷却——实时线程高频日志是纯噪声；②Trim 后冻结反馈——
    // Trim 把水位阶跃回半满，立即反馈会读到假低水位而撤销修正，随超产回贴顶形成音调锯齿；
    // ③EMA 上探——贴顶 = 产样相对消费不足（skew 偏小，前馈偏低），每次触发按 2% 步进校正，
    // 让前馈在低性能设备的大幅速度波动下仍能跟上真实速度
    if (fillLevel > kAudioOutputHighWaterThreshold)
    {
        currentInstance->trimAudioOutput();
        feedbackFreezeCycles = 10;
        if (!awaitingFirstMeasurement)
            emaMeasuredRatio *= 1.02;

        if (valveLogCooldown > 0)
            valveLogCooldown--;
        else
        {
            LOG_WARN("melonDS", "audio output high-water safety valve: fill=%d > %d, trimming to half capacity", fillLevel, kAudioOutputHighWaterThreshold);
            valveLogCooldown = 100; // ≈0.4-1 秒一条，触发段只留频率证据不留洪泛
        }
        // 水位置半满放在日志之后：日志要打的是触发时水位；本回调后续的水位反馈按半满计算
        // （TrimOutput 语义：水位一次性重置为半满，设计 §4.2）
        fillLevel = kAudioOutputHalfCapacity;
    }

    double skew;
    if (fastForward)
    {
        if (!lastFastForwardEnabled || multiplier != lastMultiplier)
        {
            // FF-on 转换或加速中倍速热更新（设计 D2 规则 4/7）：种子 + 待首测 + 缓冲余量重建。
            // EMA 同置种子并等首个新测量直接快照——从保守种子缓慢爬向真值期间持续超产，
            // 高倍速下爬升期溢出量会击穿缓冲（D2 规则 3）。种子是精心选择的初值，
            // 直接应用（不经变化率限制）；后续测量微调才受 slew 约束
            double seed = seedSkewForMultiplier(multiplier);
            emaMeasuredRatio = seed;
            awaitingFirstMeasurement = true;
            fpsAtTransition = fps.load(std::memory_order_acquire);
            awaitingFirstMeasurementCycles = 0;
            pendingBufferRebuild = true;
            // FF-on/热更新重建缓冲余量，冻结态随之作废（常速期安全阀事件的残留冻结
            // 不应泄漏进补水窗口——该期反馈方向恰为超产补水，冻结帮倒忙）
            feedbackFreezeCycles = 0;
            skew = seed;
            appliedSkew = seed;
        }
        else
        {
            bool snapshotApplied = false;
            float measuredFps = fps.load(std::memory_order_acquire);
            if (awaitingFirstMeasurement)
            {
                // fps 值变化即视为新测量到达；回调数上限兜底「测量已发生但值碰撞」的情形
                if (measuredFps != fpsAtTransition || ++awaitingFirstMeasurementCycles >= kMaxCyclesAwaitingFirstMeasurement)
                {
                    emaMeasuredRatio = measuredRatioFromFps(measuredFps);
                    awaitingFirstMeasurement = false;
                    snapshotApplied = true;
                }
            }
            else
            {
                // EMA 平滑测量窗口噪声；系数 0.5/回调（真机实测 0.25 在低性能设备的
                // 大幅速度波动下跟踪误差过大，是前馈偏低与安全阀频触的主因之一）
                emaMeasuredRatio = 0.5 * measuredRatioFromFps(measuredFps) + 0.5 * emaMeasuredRatio;
            }

            if (snapshotApplied)
            {
                // 首测快照与种子同款直接生效（不经反馈、不经 slew）：快照是测量窗口的真值，
                // 若被 ±1%/回调平滑，从种子滑向真值期间持续欠产/超产——低性能形态（真实
                // 比率≈1.0、种子 1.3）下会拖出数百毫秒的缓冲空窗硬静音；D2 规则 5 的首窗
                // 溢出余量推导同样以「快照即生效」为前提
                skew = std::clamp(emaMeasuredRatio, 1.0, 64.0);
            }
            else
            {
                // EMA 前馈目标 × 水位反馈修正，再总钳（设计 D2 规则 2/6：反馈作用在前馈目标之后、
                // 总钳之前）。反馈冻结期（安全阀 Trim 后）factor 取 1——Trim 造成的假低水位
                // 不参与修正，等真实水位重新形成
                double target = emaMeasuredRatio;
                if (feedbackFreezeCycles > 0)
                    feedbackFreezeCycles--;
                else
                    target *= fillFeedbackFactor(fillLevel);

                // 变化率限制：目标值平滑到达，消除音调阶跃与 blip 帧内密度断裂（常速分支、
                // 转换分支与首测快照不受此约束——前两者设计锁定直接生效，快照是测量真值）
                skew = std::clamp(slewLimitSkew(target, appliedSkew), 1.0, 64.0);
            }
        }
    }
    else
    {
        // 常速分支与改动前逐位一致（回归隔离，设计 §7.4）；加速→关的转换不特殊处理：
        // skew 立即回常速值、缓冲不动（D3——残留加速期内容以密度连续方式滑回正常音调）
        skew = std::clamp(60.0 / INTERNAL_FRAME_RATE, 0.995, 1.005);
    }

    lastFastForwardEnabled = fastForward;
    lastMultiplier = multiplier;
    appliedSkew = skew;

    currentInstance->setAudioOutputSkew(skew);

#ifndef NDEBUG
    // 运行时探针（设计 §7.3 P-fill 的落码）：debug 构建记录低水位事件（全容量 15% 观察线，
    // 与 D4 口径一致），供真机标定与 underrun 诊断。放在读取输出之前——缓冲读空（fill=0，
    // 最需诊断的形态）会在下方走硬零提前返回，探针必须先于它执行。高水位越界由安全阀 WARN 记录
    if (fillLevel < (int) (kAudioOutputFullCapacity * 0.15))
        LOG_DEBUG("melonDS", "audio-probe low fill: fill=%d ff=%d fps=%.1f ema=%.3f skew=%.3f",
                  fillLevel, fastForward, fps.load(std::memory_order_relaxed), emaMeasuredRatio, skew);
#endif

    int num_in = currentInstance->readAudioOutput((s16*) audioData, numFrames);

    // 缓冲余量重建（设计 D2 规则 4）：动作次序固定为先读取后重建——先清后读会让本回调整段输出硬零。
    // 固定档仅在水位高于半容量时 Trim：TrimOutput 无条件把水位重置为半满，水位 ≤ 半容量时余量已足够，
    // 执行反而会把读指针向回拨、重播已消费样本；不限速档全清，为首个 3 帧测量窗口留出溢出余量
    if (pendingBufferRebuild)
    {
        if (multiplier > 0)
        {
            if (currentInstance->getAudioOutputSize() > kAudioOutputHalfCapacity)
                currentInstance->trimAudioOutput();
        }
        else
            currentInstance->drainAudioOutput();

        pendingBufferRebuild = false;
    }

    if (num_in < 1)
    {
        memset(audioData, 0, numFrames * sizeof(s16) * 2);
        return oboe::DataCallbackResult::Continue;
    }

    if (_volume < 256)
    {
        s16* samples = (s16*) audioData;
        for (int i = 0; i < num_in * 2; i++)
            samples[i] = ((s32) samples[i] * _volume) >> 8;
    }

    if (num_in < numFrames)
    {
        int last = num_in - 1;

        for (int i = num_in; i < numFrames; i++)
            ((u32*)audioData)[i] = ((u32*)audioData)[last];
    }

    // 阈值静音判定（设计 D7/U4）：输出侧过滤器，与速率匹配正交——读取与 skew 计算照常执行，
    // 静音不停速率匹配。迟滞双线：静音线 = 阈值、恢复线 = 0.8 × 阈值（实测比率围绕阈值波动时
    // 不反复切换 = 无连续咔哒）。判定输入是 EMA 裸值（不含水位反馈与总钳，那两个为水位控制服务）
    float muteThreshold = fastForwardAudioMuteThreshold.load(std::memory_order_relaxed);
    if (!fastForward || muteThreshold <= 0.0f)
    {
        // 非加速期恒不静音（标志翻转为关立即复位，无静音残留）；「从不」档（0，缺省）恒不触发，
        // 回调路径零行为差异（U4-5）
        audioMuted = false;
    }
    else if (audioMuted)
    {
        audioMuted = emaMeasuredRatio > muteThreshold * 0.8;
    }
    else
    {
        audioMuted = emaMeasuredRatio > muteThreshold;
    }

    // 静音置零与录制分支均非热点，不加优化提示属性——构建链声明 C++17（app/build.gradle.kts
    // cppFlags），unlikely 是 C++20 属性，旧版 clang 会报未知属性新警告
    if (audioMuted)
        memset(audioData, 0, numFrames * sizeof(s16) * 2);

    if (_recordingStream)
        _recordingStream->write((char*) audioData, numFrames * sizeof(s16) * 2);

    return oboe::DataCallbackResult::Continue;
}

void OboeCallback::onErrorAfterClose(oboe::AudioStream* stream, oboe::Result result)
{
    if (result == oboe::Result::ErrorDisconnected && onErrorCallback != nullptr) {
        onErrorCallback();
    }
}
