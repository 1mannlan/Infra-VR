#pragma once
// Lightweight CPU frame profiler for INFRA VR (toggle with F12 on the desktop keyboard).
// Collects per-frame CPU timings of the VR render pipeline for a fixed window and writes
// percentile statistics to VR\profile_YYYYMMDD_HHMMSS.txt. Samples are added from the main thread
// (dRenderView) and the render thread (PresentEx with mat_queue_mode 2), so access is mutex-protected.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

class VRProfiler
{
public:
    using Clock = std::chrono::steady_clock;

    enum Stat
    {
        FrameInterval = 0,   // dRenderView -> dRenderView (full game frame on the main thread)
        RenderViewTotal,     // whole dRenderView hook (both eyes + VR setup)
        PreEyes,             // VR setup before Eye 1 (tracking, traces, patches)
        Eye1,                // CViewRender::RenderView for the left eye
        Eye2,                // CViewRender::RenderView for the right eye
        PostEyes,            // cleanup after Eye 2
        VRUpdateTotal,       // VR::MainThreadUpdate() (input, tracking, snapshot)
        Submit,              // render thread: BeginVRSubmit (flush + CS thread sync) + compositor Submit
        WaitGetPoses,        // render thread: IVRCompositor::WaitGetPoses (blocking wait, not CPU work)
        StatCount
    };

    static double Ms(Clock::time_point a, Clock::time_point b)
    {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }

    bool Active() const { return m_Active.load(); }

    void Start(double seconds)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto& s : m_Samples)
        {
            s.clear();
            s.reserve(4096);
        }
        m_DurationSec = seconds;
        m_Start = Clock::now();
        m_Active = true;
    }

    void Add(Stat stat, double ms)
    {
        if (!m_Active.load() || stat < 0 || stat >= StatCount)
            return;
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Active.load())
            m_Samples[stat].push_back((float)ms);
    }

    bool ShouldFinish() const
    {
        return m_Active.load() && Ms(m_Start, Clock::now()) >= m_DurationSec * 1000.0;
    }

    // Writes the report and returns the file path (empty on failure).
    std::string Finish(int renderWidth, int renderHeight, int antiAliasing, bool queuedRendering)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Active = false;

        std::time_t now = std::time(nullptr);
        std::tm tmNow {};
        localtime_s(&tmNow, &now);
        char path[128];
        std::strftime(path, sizeof(path), "VR\\profile_%Y%m%d_%H%M%S.txt", &tmNow);

        FILE* f = nullptr;
        if (fopen_s(&f, path, "w") != 0 || !f)
            return std::string();

        static const char* s_Names[StatCount] = {
            "FrameInterval (main thread)",
            "RenderView hook total",
            "  VR setup before eyes",
            "  Eye 1 RenderView",
            "  Eye 2 RenderView",
            "  Cleanup after eyes",
            "VR main-thread update",
            "  Submit (+flush/sync)",
            "  WaitGetPoses (blocking)"
        };

        const auto& frames = m_Samples[FrameInterval];
        size_t over83 = 0, over111 = 0;
        for (float v : frames)
        {
            if (v > 8.33f) ++over83;
            if (v > 11.1f) ++over111;
        }

        fprintf(f, "INFRA VR CPU profile\n");
        fprintf(f, "Duration: %.1f s, frames: %zu, eye RT: %dx%d, MSAA: %dx\n",
                m_DurationSec, frames.size(), renderWidth, renderHeight, antiAliasing);
        fprintf(f, "Threading: %s\n", queuedRendering ? "multi-threaded (mat_queue_mode 2, separate render thread)" : "single-threaded (mat_queue_mode 0)");
        if (!frames.empty())
            fprintf(f, "Frames > 8.33 ms (120 Hz): %.1f %%, > 11.1 ms (90 Hz): %.1f %%\n",
                    100.0 * over83 / frames.size(), 100.0 * over111 / frames.size());
        fprintf(f, "\n%-30s %8s %8s %8s %8s %8s %8s\n", "Stat [ms]", "avg", "p50", "p95", "p99", "max", "count");

        for (int i = 0; i < StatCount; ++i)
        {
            std::vector<float> v = m_Samples[i];
            if (v.empty())
            {
                fprintf(f, "%-30s %8s\n", s_Names[i], "-");
                continue;
            }
            std::sort(v.begin(), v.end());
            double sum = 0.0;
            for (float x : v) sum += x;
            auto pct = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1)))]; };
            fprintf(f, "%-30s %8.2f %8.2f %8.2f %8.2f %8.2f %8zu\n",
                    s_Names[i], sum / v.size(), pct(0.50), pct(0.95), pct(0.99), v.back(), v.size());
        }

        fprintf(f, "\nNotes:\n");
        fprintf(f, "- Single-threaded: FrameInterval minus WaitGetPoses ~= real CPU work per frame.\n");
        fprintf(f, "- Multi-threaded: Eye RenderView only records commands; Submit/WaitGetPoses run on the render thread.\n");
        fprintf(f, "- Source engine vprof hierarchy for the same window: infra\\vr_vprof.log\n");
        fclose(f);
        return std::string(path);
    }

private:
    std::atomic<bool> m_Active { false };
    std::mutex m_Mutex;
    double m_DurationSec = 10.0;
    Clock::time_point m_Start {};
    std::vector<float> m_Samples[StatCount];
};
