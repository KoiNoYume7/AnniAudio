#include "GroupBus.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio_utils.hpp"

namespace anniaudio::core {

GroupBus::GroupBus() = default;
GroupBus::~GroupBus() = default;

void GroupBus::addInput(InputProcessor& input)
{
    std::lock_guard<std::mutex> lk(sourceMutex_);
    for (const auto& s : sources_) {
        if (s->input == &input) return;
    }
    auto s = std::make_shared<Source>();
    s->input = &input;
    s->readCursor = input.outputRing().head();
    sources_.push_back(s);
}

void GroupBus::removeInput(InputProcessor& input)
{
    std::lock_guard<std::mutex> lk(sourceMutex_);
    sources_.erase(std::remove_if(sources_.begin(), sources_.end(),
        [&input](const std::shared_ptr<Source>& s) { return s->input == &input; }),
        sources_.end());
}

void GroupBus::clearInputs()
{
    std::lock_guard<std::mutex> lk(sourceMutex_);
    sources_.clear();
}

void GroupBus::setGain(float gain)
{
    gain_.store(gain);
}

void GroupBus::setMuted(bool muted)
{
    muted_.store(muted);
}

float GroupBus::gain() const
{
    return gain_.load();
}

bool GroupBus::muted() const
{
    return muted_.load();
}

float GroupBus::peak() const
{
    return peak_.load();
}

float GroupBus::rms() const
{
    return rms_.load();
}

void GroupBus::mix(float* dst, uint32_t frames, uint32_t rate, uint32_t channels)
{
    const size_t totalSamples = static_cast<size_t>(frames) * channels;
    if (totalSamples == 0) return;

    std::fill_n(dst, totalSamples, 0.0f);

    if (muted_.load()) return;

    // Snapshot the source list. Each Source is kept alive by the shared_ptr for
    // the duration of this mix, so the matrix can remove it from the active list
    // without freeing it mid-mix.
    std::vector<std::shared_ptr<Source>> local;
    {
        std::lock_guard<std::mutex> lk(sourceMutex_);
        local = sources_;
    }

    const uint32_t srcCh = InputProcessor::kProcessingChannels;
    const size_t needSamples = static_cast<size_t>(frames) * srcCh;

    for (auto& s : local) {
        if (!s || !s->input) continue;
        auto& ring = s->input->outputRing();
        if (s->srcBuf.size() < needSamples)
            s->srcBuf.resize(needSamples + static_cast<size_t>(srcCh) * 2);

        size_t gotSamples = ring.readOrSilence(s->srcBuf.data(), needSamples, s->readCursor);
        uint32_t gotFrames = static_cast<uint32_t>(gotSamples / srcCh);
        if (gotFrames == 0) continue;

        const float* src = s->srcBuf.data();
        if (channels == 2 && srcCh == 2) {
            for (uint32_t f = 0; f < gotFrames; ++f) {
                dst[f * 2 + 0] += src[f * 2 + 0];
                dst[f * 2 + 1] += src[f * 2 + 1];
            }
        } else if (channels == 1) {
            for (uint32_t f = 0; f < gotFrames; ++f) {
                dst[f] += (src[f * 2 + 0] + src[f * 2 + 1]) * 0.5f;
            }
        } else {
            for (uint32_t f = 0; f < gotFrames; ++f) {
                for (uint32_t c = 0; c < channels; ++c) {
                    float v = (c < srcCh) ? src[f * srcCh + c] : 0.0f;
                    dst[f * channels + c] += v;
                }
            }
        }
    }

    float g = gain_.load();
    if (g != 1.0f) {
        for (size_t i = 0; i < totalSamples; ++i) dst[i] *= g;
    }

    // Update peak/rms meters.
    float maxAbs = 0.0f;
    float sumSq = 0.0f;
    for (size_t i = 0; i < totalSamples; ++i) {
        float a = std::fabs(dst[i]);
        if (a > maxAbs) maxAbs = a;
        sumSq += dst[i] * dst[i];
    }
    peak_.store(maxAbs);
    rms_.store(totalSamples > 0 ? std::sqrt(sumSq / static_cast<float>(totalSamples)) : 0.0f);

    (void)rate; // OutputMixer is fixed at 48 kHz; rate is ignored.
}

std::vector<InputProcessor*> GroupBus::inputProcessors() const
{
    std::lock_guard<std::mutex> lk(sourceMutex_);
    std::vector<InputProcessor*> out;
    out.reserve(sources_.size());
    for (const auto& s : sources_) {
        if (s && s->input) out.push_back(s->input);
    }
    return out;
}

} // namespace anniaudio::core
