#include "frame_pipeline.hpp"

#include <chrono>
#include <vector>

#include "display_renderer.hpp"
#include "h264_decoder.hpp"
#include "log.hpp"
#include "messages.hpp"

namespace {
constexpr long long kStallMs = 2000;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

FramePipeline::FramePipeline(DisplayRenderer& renderer, QObject* parent)
    : QObject(parent), renderer_(renderer) {}

FramePipeline::~FramePipeline() {
    stop();
}

bool FramePipeline::start(const std::string& device_id, int width, int height,
                          std::function<void()> on_stall) {
    stop();
    auto decoder = std::make_unique<H264Decoder>();
    if (!decoder->initialize(width, height)) {
        mlog::error("Decoder init failed for " + device_id);
        return false;
    }
    decoder_ = std::move(decoder);
    on_stall_ = std::move(on_stall);
    decoded_.store(0);
    last_good_ms_ = now_ms();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        has_pending_ = false;
        quit_ = false;
    }
    thread_ = std::thread(&FramePipeline::run, this);
    return true;
}

void FramePipeline::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    decoder_.reset();
}

void FramePipeline::push(QByteArray payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (quit_) {
        return;
    }
    // Overwrite: an undecoded frame is already obsolete.
    pending_ = std::move(payload);
    has_pending_ = true;
    cv_.notify_one();
}

void FramePipeline::run() {
    for (;;) {
        QByteArray payload;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || has_pending_; });
            if (quit_) {
                return;
            }
            payload.swap(pending_);
            has_pending_ = false;
        }

        std::vector<uint8_t> bytes(payload.begin(), payload.end());
        proto::VideoFrameInfo info;
        if (!proto::parse_video_frame_payload(bytes, info)) {
            mlog::warn("Malformed VideoFrame payload");
            continue;
        }

        QImage frame;
        if (decoder_ && decoder_->decode(info.data, info.size, frame)) {
            last_good_ms_ = now_ms();
            decoded_.fetch_add(1);
            renderer_.set_frame(frame);
            emit frame_ready();
        } else if (now_ms() - last_good_ms_ > kStallMs) {
            // Missing keyframe after loss: ask the agent for a fresh one.
            last_good_ms_ = now_ms();
            if (on_stall_) {
                on_stall_();
            }
        }
    }
}
