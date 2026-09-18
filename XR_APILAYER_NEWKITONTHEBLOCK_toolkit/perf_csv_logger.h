// MIT License
//
// Copyright(c) 2026 Tsevopolus
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

// Writes the toolkit's own per-frame statistics to a CSV file for offline analysis.
//
// This does not measure anything new: it piggybacks on the same MenuStatistics the
// Developer overlay already displays, so enabling it costs one formatted line per
// statistics update (roughly once per second, not once per frame) plus a buffered
// file write. It is off unless explicitly enabled.
//
// Files land next to the toolkit's own logs:
//   %LocalAppData%\OpenXR-Toolkit\logs\perf-<layer>-<timestamp>.csv
//
// Open in Excel/LibreOffice, or diff two sessions (e.g. this fork vs. the original
// 1.3.2 layer) to see where frame time actually goes.

namespace toolkit::utilities {

    class PerformanceCsvLogger {
      public:
        PerformanceCsvLogger() = default;

        ~PerformanceCsvLogger() {
            stop();
        }

        bool isLogging() const {
            return m_file.is_open();
        }

        // Starts a new CSV file. Returns the path on success, empty on failure.
        // layerName is used in the filename so fork-vs-original runs stay distinguishable.
        std::string start(const std::string& layerName) {
            stop();

            try {
                const auto logsDir = std::filesystem::path(getenv("LOCALAPPDATA")) / "OpenXR-Toolkit" / "logs";
                std::filesystem::create_directories(logsDir);

                // Timestamped so repeated runs never overwrite each other.
                const auto now = std::time(nullptr);
                std::tm tm{};
                localtime_s(&tm, &now);
                char stamp[32]{};
                std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);

                const auto path = logsDir / fmt::format("perf-{}-{}.csv", layerName, stamp);
                m_file.open(path, std::ios::out | std::ios::trunc);
                if (!m_file.is_open()) {
                    return {};
                }

                m_startTime = std::chrono::steady_clock::now();
                writeHeader();
                m_path = path.string();
                return m_path;

            } catch (const std::exception& exc) {
                toolkit::log::Log("Failed to start performance CSV log: %s\n", exc.what());
                stop();
                return {};
            }
        }

        void stop() {
            if (m_file.is_open()) {
                m_file.flush();
                m_file.close();
            }
            m_path.clear();
        }

        const std::string& getPath() const {
            return m_path;
        }

        // Call from updateStatistics(). Silently does nothing when not logging.
        void log(const menu::MenuStatistics& stats) {
            if (!m_file.is_open()) {
                return;
            }

            try {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - m_startTime)
                                         .count();

                // Times are microseconds as stored; emitted as-is so no precision is lost
                // in the file. Convert to ms in the spreadsheet if you prefer.
                m_file << elapsed << ',' << fmt::format("{:.2f}", stats.fps) << ',' << stats.appCpuTimeUs << ','
                       << stats.appGpuTimeUs << ',' << stats.renderCpuTimeUs << ',' << stats.endFrameCpuTimeUs << ','
                       << stats.waitCpuTimeUs << ',' << stats.processorGpuTimeUs[0] << ','
                       << stats.processorGpuTimeUs[1] << ',' << stats.overlayCpuTimeUs << ',' << stats.overlayGpuTimeUs
                       << ',' << stats.handTrackingCpuTimeUs << ',' << stats.predictionTimeUs << ','
                       << stats.vramUsedSize << ',' << (uint32_t)stats.vramUsedPercent << ','
                       << stats.actualRenderWidth << ',' << stats.numRenderTargetsWithVRS << ','
                       << stats.numBiasedSamplers << ',' << fmt::format("{:.3f}", stats.icd) << ','
                       << (stats.isFramePipeliningDetected ? 1 : 0) << '\n';

                // Flushed every line on purpose: a VR session that ends in a crash or a
                // hard exit should still leave a complete, readable file behind - that is
                // exactly the case worth analyzing. At roughly one line per second the
                // cost is irrelevant.
                m_file.flush();

            } catch (const std::exception& exc) {
                toolkit::log::Log("Error writing performance CSV log: %s\n", exc.what());
                stop();
            }
        }

      private:
        void writeHeader() {
            m_file << "elapsed_ms,fps,"
                      "app_cpu_us,app_gpu_us,render_cpu_us,layer_cpu_us,wait_cpu_us,"
                      "upscaler_gpu_us,postprocess_gpu_us,overlay_cpu_us,overlay_gpu_us,"
                      "handtracking_cpu_us,prediction_us,"
                      "vram_used_bytes,vram_used_percent,actual_render_width,"
                      "rendertargets_with_vrs,biased_samplers,icd,frame_pipelining\n";
            m_file.flush();
        }

        std::ofstream m_file;
        std::string m_path;
        std::chrono::steady_clock::time_point m_startTime;
    };

} // namespace toolkit::utilities
