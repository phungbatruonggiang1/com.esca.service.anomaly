// esca-anomalyd: listens, cuts the stream into segments, asks the server
// what it heard, and puts the answer on the bus.
//
// The same split as esca-visiond on the camera side: a worker reads the
// shared-memory ring while the main thread runs the D-Bus loop, and results
// go out as a signal carrying the sequence number of the audio they
// describe. Nothing here holds up the recording: if the server is slow or
// gone, segments are dropped and the fact is reported.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "esca/ai/client.hpp"
#include "esca/audio/client.hpp"

namespace {

constexpr const char *kServiceName = "vn.sparc.esca.Anomaly1";
constexpr const char *kObjectPath = "/vn/sparc/esca/Anomaly1";
constexpr const char *kInterface = "vn.sparc.esca.Anomaly1";

sdbus::IConnection *g_connection = nullptr;

void handle_signal(int)
{
    if (g_connection)
        g_connection->leaveEventLoop();
}

struct Options {
    std::string server = "http://192.168.1.10:8099";
    std::string device;
    std::string machine = "unknown";
    bool autostart = true;
    int timeout_ms = 5000;
};

int usage()
{
    std::fprintf(stderr,
                 "usage: esca-anomalyd [options]\n"
                 "  --server URL     AI server base URL\n"
                 "  --device ID      ALSA device, empty for the first one\n"
                 "  --machine NAME   reported with each verdict\n"
                 "  --timeout MS     HTTP timeout (default 5000)\n"
                 "  --no-autostart   wait for Start() instead of listening at once\n");
    return 2;
}

class AnomalyService {
public:
    AnomalyService(sdbus::IConnection &connection, Options options)
        : options_(std::move(options)),
          ai_(esca::ai::ServerConfig { options_.server, options_.timeout_ms, 2000 })
    {
        object_ = sdbus::createObject(connection, sdbus::ObjectPath { kObjectPath });

        object_->addVTable(
            sdbus::registerMethod("Start").implementedAs([this] { return start(); }),
            sdbus::registerMethod("Stop").implementedAs([this] { return stop(); }),
            sdbus::registerMethod("GetState").implementedAs([this] { return state(); }),
            sdbus::registerMethod("GetErrorMessage").implementedAs([this] {
                std::lock_guard<std::mutex> lock(mutex_);
                return last_error_;
            }),
            // sequence, timestamp, anomaly, score, threshold, inference ms
            sdbus::registerSignal("AnomalyEvent")
                .withParameters<uint64_t, uint64_t, bool, double, double, double>(),
            sdbus::registerSignal("FaultEvent").withParameters<std::string, std::string>())
            .forInterface(kInterface);
    }

    ~AnomalyService()
    {
        // Safe to join here, unlike inside a method handler: the event loop
        // has left, so a signal the worker is still emitting cannot be
        // waiting on a lock this thread holds. Joining from inside Stop()
        // is the deadlock the vision service had to be fixed for.
        running_ = false;
        if (worker_.joinable())
            worker_.join();
    }

    /// Ask the server what it wants before recording anything at that rate.
    bool prepare(std::string &error)
    {
        if (!ai_.fetch_spec(spec_, error))
            return false;
        std::fprintf(stderr,
                     "esca-anomalyd: server wants %u Hz, %u ch, %.1f s (%u samples), "
                     "model %s, threshold %g\n",
                     spec_.sample_rate, spec_.channels, spec_.seconds, spec_.samples,
                     spec_.model.c_str(), spec_.threshold);
        return true;
    }

    int32_t start()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_)
            return 0;
        if (worker_alive_) {
            last_error_ = "the previous run has not finished yet";
            return -1;
        }
        if (worker_.joinable())
            worker_.join();

        running_ = true;
        worker_alive_ = true;
        worker_ = std::thread([this] {
            run();
            worker_alive_ = false;
        });
        return 0;
    }

    int32_t stop()
    {
        // Only lower the flag. See the destructor.
        running_ = false;
        return 0;
    }

    sdbus::Struct<bool, std::string, uint32_t, uint64_t, uint64_t> state()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return { running_.load(), spec_.model, spec_.sample_rate, sent_.load(),
                 dropped_.load() };
    }

private:
    void fault(const std::string &where, const std::string &what)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = what;
        }
        std::fprintf(stderr, "esca-anomalyd: %s: %s\n", where.c_str(), what.c_str());
        try {
            object_->emitSignal(sdbus::SignalName { "FaultEvent" })
                .onInterface(sdbus::InterfaceName { kInterface })
                .withArguments(where, what);
        } catch (const sdbus::Error &) {
        }
    }

    void run()
    {
        using namespace esca::audio;

        AudioClient audio;
        std::string error;
        if (audio.open(options_.device, error) < 0) {
            fault("audio", "open: " + error);
            running_ = false;
            return;
        }

        PcmFormat wanted;
        wanted.rate = spec_.sample_rate;
        wanted.channels = spec_.channels ? spec_.channels : 1;
        wanted.bits = 16;
        wanted.frames_per_chunk = spec_.sample_rate / 10;
        audio.set_format(wanted, error);

        if (!audio.start(error)) {
            fault("audio", "start: " + error);
            running_ = false;
            return;
        }

        const PcmFormat format = audio.stream_format();
        if (format.rate != spec_.sample_rate) {
            fault("audio", "the microphone gave " + std::to_string(format.rate)
                               + " Hz but the model needs "
                               + std::to_string(spec_.sample_rate)
                               + " Hz; verdicts would be meaningless");
            audio.stop();
            audio.close();
            running_ = false;
            return;
        }

        const size_t segment_bytes =
            static_cast<size_t>(spec_.samples) * format.bytes_per_frame();
        std::fprintf(stderr, "esca-anomalyd: listening on %u Hz, %.1f s segments\n",
                     format.rate, spec_.seconds);

        std::vector<uint8_t> segment;
        segment.reserve(segment_bytes);
        std::vector<uint8_t> chunk;
        ChunkInfo info;
        uint64_t segment_seq = 0;
        bool starved = false;

        while (running_) {
            if (!audio.next_chunk(chunk, info, 2000)) {
                if (!starved) {
                    fault("audio", "timed out waiting for audio");
                    starved = true;
                }
                continue;
            }
            starved = false;

            if (segment.empty())
                segment_seq = info.sequence;
            segment.insert(segment.end(), chunk.begin(), chunk.end());
            if (segment.size() < segment_bytes)
                continue;
            segment.resize(segment_bytes);

            esca::ai::Verdict verdict;
            if (ai_.infer(segment.data(), segment.size(), format.rate, segment_seq, verdict,
                          error)) {
                ++sent_;
                try {
                    object_->emitSignal(sdbus::SignalName { "AnomalyEvent" })
                        .onInterface(sdbus::InterfaceName { kInterface })
                        .withArguments(verdict.sequence, info.timestamp_ns, verdict.anomaly,
                                       verdict.score, verdict.threshold,
                                       verdict.inference_ms);
                } catch (const sdbus::Error &e) {
                    std::fprintf(stderr, "esca-anomalyd: cannot emit: %s\n",
                                 e.getMessage().c_str());
                }
                if (verdict.anomaly) {
                    std::fprintf(stderr, "esca-anomalyd: seq %llu ANOMALY score %.5f\n",
                                 static_cast<unsigned long long>(verdict.sequence),
                                 verdict.score);
                }
            } else {
                // Dropped on purpose. A verdict about sound from a minute
                // ago helps nobody, so there is no retry queue on this path
                // -- unlike uploads, where losing data is the worse failure.
                ++dropped_;
                if (dropped_ % 10 == 1)
                    fault("server", error);
            }

            segment.clear();
        }

        audio.stop();
        audio.close();
        running_ = false;
        std::fprintf(stderr, "esca-anomalyd: stopped after %llu verdict(s), %llu dropped\n",
                     static_cast<unsigned long long>(sent_.load()),
                     static_cast<unsigned long long>(dropped_.load()));
    }

    Options options_;
    esca::ai::AiClient ai_;
    esca::ai::ModelSpec spec_;
    std::unique_ptr<sdbus::IObject> object_;

    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> running_ { false };
    std::atomic<bool> worker_alive_ { false };
    std::atomic<uint64_t> sent_ { 0 };
    std::atomic<uint64_t> dropped_ { 0 };
    std::string last_error_;
};

} // namespace

int main(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };

        if (arg == "--server") options.server = next();
        else if (arg == "--device") options.device = next();
        else if (arg == "--machine") options.machine = next();
        else if (arg == "--timeout") options.timeout_ms = std::stoi(next());
        else if (arg == "--no-autostart") options.autostart = false;
        else return usage();
    }

    try {
        auto connection = sdbus::createSystemBusConnection(sdbus::ServiceName { kServiceName });
        g_connection = connection.get();

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        AnomalyService service(*connection, options);

        std::string error;
        if (!service.prepare(error)) {
            // Not fatal: the server may come up later, and Start() tries
            // again. Refusing to take the bus name would only hide the
            // problem from whoever asks what is wrong.
            std::fprintf(stderr, "esca-anomalyd: cannot reach %s: %s\n",
                         options.server.c_str(), error.c_str());
        } else if (options.autostart) {
            service.start();
        }

        std::fprintf(stderr, "esca-anomalyd: ready on %s\n", kServiceName);
        connection->enterEventLoop();
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "esca-anomalyd: fatal: %s\n", e.what());
        return 1;
    }
}
