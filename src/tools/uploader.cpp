// esca-uploader: ships a collected corpus to the AI server.
//
//   esca-uploader --server http://192.168.1.10:8099 --corpus /var/lib/esca/corpus
//
// Reads the manifest the recorder wrote, uploads what has not gone yet, and
// records what succeeded. Nothing is deleted here: disk is cheap next to a
// corpus that cannot be recorded again, and a second copy on the server is
// not a reason to destroy the first. --delete-sent is there for when the SD
// card fills.

#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "esca/ai/client.hpp"

namespace {

int usage()
{
    std::fprintf(stderr,
                 "usage: esca-uploader [options]\n"
                 "  --server URL      AI server base URL\n"
                 "  --corpus DIR      corpus root (default /var/lib/esca/corpus)\n"
                 "  --delete-sent     remove local clips the server confirmed\n"
                 "  --dry-run         list what would be sent\n");
    return 2;
}

} // namespace

int main(int argc, char **argv)
{
    std::string server = "http://192.168.1.10:8099";
    std::string corpus = "/var/lib/esca/corpus";
    bool delete_sent = false;
    bool dry_run = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--server") server = next();
        else if (arg == "--corpus") corpus = next();
        else if (arg == "--delete-sent") delete_sent = true;
        else if (arg == "--dry-run") dry_run = true;
        else return usage();
    }

    const std::string manifest_path = corpus + "/manifest.jsonl";
    const std::string sent_path = corpus + "/uploaded.txt";

    // Which clips already went. A plain list of paths, so an interrupted run
    // resumes rather than sending everything twice.
    std::set<std::string> already;
    {
        std::ifstream in(sent_path);
        std::string line;
        while (std::getline(in, line))
            if (!line.empty())
                already.insert(line);
    }

    std::ifstream manifest(manifest_path);
    if (!manifest) {
        std::fprintf(stderr, "cannot read %s\n", manifest_path.c_str());
        return 1;
    }

    esca::ai::AiClient client(esca::ai::ServerConfig{ server, 30000, 3000 });
    std::string error;
    if (!dry_run && !client.healthy(error)) {
        std::fprintf(stderr, "server %s is not answering: %s\n", server.c_str(),
                     error.c_str());
        return 1;
    }

    std::ofstream sent(sent_path, std::ios::app);
    int uploaded = 0, skipped = 0, failed = 0;
    std::string line;

    while (std::getline(manifest, line)) {
        if (line.empty())
            continue;

        nlohmann::json row;
        try {
            row = nlohmann::json::parse(line);
        } catch (const nlohmann::json::exception &) {
            continue; // a half-written line from a session that was killed
        }

        const std::string relative = row.value("file", "");
        if (relative.empty() || already.count(relative)) {
            ++skipped;
            continue;
        }

        const std::string path = corpus + "/" + relative;
        if (dry_run) {
            std::printf("would send %s\n", relative.c_str());
            ++uploaded;
            continue;
        }

        esca::ai::ClipMeta meta;
        meta.machine = row.value("machine", "unknown");
        meta.label = row.value("label", "unlabelled");
        meta.device = row.value("device", "");
        meta.started_ns = row.value("started_ns", 0ull);
        meta.dropped = row.value("dropped", 0ull);

        if (client.upload(path, meta, error)) {
            // Written before any deletion, so a crash here costs a duplicate
            // upload rather than a lost clip.
            sent << relative << "\n";
            sent.flush();
            already.insert(relative);
            ++uploaded;
            if (delete_sent)
                ::unlink(path.c_str());
        } else {
            std::fprintf(stderr, "%s: %s\n", relative.c_str(), error.c_str());
            ++failed;
        }
    }

    std::printf("%d uploaded, %d already sent, %d failed\n", uploaded, skipped, failed);
    return failed ? 1 : 0;
}
