#pragma once

#include <QByteArray>
#include <QObject>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class H264Decoder;
class DisplayRenderer;

// Decodes video payloads on a dedicated thread and publishes only the newest
// picture to the renderer. The GUI thread never runs the decoder, and a slow
// GUI drops stale frames instead of queueing an ever-growing backlog.
class FramePipeline : public QObject {
    Q_OBJECT

public:
    explicit FramePipeline(DisplayRenderer& renderer, QObject* parent = nullptr);
    ~FramePipeline() override;

    // on_stall runs on the decode thread when nothing decoded for a while.
    bool start(const std::string& device_id, int width, int height,
               std::function<void()> on_stall);
    void stop();

    // Called from the tunnel session thread; keeps only the latest payload.
    void push(QByteArray payload);

    // How old the pictures that finished decoding in the last window were, as
    // the raw difference between our clock at decode and the agent's at capture.
    // The window is summed rather than sampled: the newest frame of a second says
    // more about when a frame happened to land than about how old they are.
    struct LatencyWindow {
        int64_t diff_sum_us = 0;
        uint64_t frames = 0;
    };
    // Read once and cleared, so a desktop that has gone quiet reports no frames
    // rather than repeating one stale answer.
    LatencyWindow exchange_window();

    uint64_t exchange_decoded() { return decoded_.exchange(0); }

signals:
    // Emitted on the decode thread; connected queued so the GUI thread only
    // has to repaint. Carries no pixel data.
    void frame_ready();

private:
    void run();

    DisplayRenderer& renderer_;
    std::unique_ptr<H264Decoder> decoder_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    QByteArray pending_;
    bool has_pending_ = false;
    bool quit_ = false;
    std::function<void()> on_stall_;
    std::atomic<uint64_t> decoded_{0};
    std::mutex window_mutex_;
    LatencyWindow window_;
    long long last_good_ms_ = 0;
};
