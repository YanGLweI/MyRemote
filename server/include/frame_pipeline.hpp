#pragma once

#include <QByteArray>
#include <QObject>

#include "video_decoder.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
               CodecType codec_type, std::function<void()> on_stall);
    void stop();

    // Called from the tunnel session thread; keeps only the latest payload.
    void push(QByteArray payload);

    uint64_t exchange_decoded() { return decoded_.exchange(0); }

signals:
    // Emitted on the decode thread; connected queued so the GUI thread only
    // has to repaint. Carries no pixel data.
    void frame_ready();

private:
    void run();

    DisplayRenderer& renderer_;
    std::unique_ptr<IVideoDecoder> decoder_;
    CodecType codec_type_ = CodecType::CODEC_H264;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    QByteArray pending_;
    bool has_pending_ = false;
    bool quit_ = false;
    std::function<void()> on_stall_;
    std::atomic<uint64_t> decoded_{0};
    long long last_good_ms_ = 0;
};
