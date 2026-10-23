#include "slic3r/Utils/Http.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    std::mutex                     mutex;
    std::condition_variable        changed;
    std::atomic<int>               successes{0};
    std::atomic<int>               started{0};
    std::atomic<int>               callbacks{0};
    std::atomic<int>               cancelled{0};
    std::vector<Slic3r::Http::Ptr> requests;
    const std::string              url = argv[1];
    try {
        // A completed callback may enqueue another request; shutdown must not
        // join workers while holding a mutex needed by this path.
        Slic3r::Http::get(url + "/ok")
            .on_complete([&](std::string, unsigned) {
                ++successes;
                Slic3r::Http::get(url + "/ok")
                    .on_complete([&](std::string, unsigned) {
                        const std::lock_guard<std::mutex> lock(mutex);
                        ++successes;
                        changed.notify_all();
                    })
                    .perform();
            })
            .perform();
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!changed.wait_for(lock, 15s, [&] { return successes.load() == 2; }))
                throw std::runtime_error("Chained async completion did not finish");
        }
        for (int i = 0; i < 6; ++i) {
            auto seen = std::make_shared<std::atomic<bool>>(false);
            requests.push_back(Slic3r::Http::get(url + "/slow")
                                   .on_progress([&, seen](Slic3r::Http::Progress p, bool& cancellation) {
                                       ++callbacks;
                                       if (cancellation) {
                                           ++cancelled;
                                           // A shutdown callback may try to schedule more work.
                                           // This must be rejected without deadlocking the join.
                                           Slic3r::Http::get(url + "/late").perform();
                                       }
                                       if (p.dlnow > 0 && !seen->exchange(true)) {
                                           const std::lock_guard<std::mutex> lock(mutex);
                                           ++started;
                                           changed.notify_all();
                                       }
                                   })
                                   .perform());
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!changed.wait_for(lock, 15s, [&] { return started.load() == 6; }))
                throw std::runtime_error("Concurrent HTTPS transfers did not start");
        }
        const auto start = std::chrono::steady_clock::now();
        Slic3r::Http::shutdown_async();
        if (std::chrono::steady_clock::now() - start > 10s)
            throw std::runtime_error("Cancellation waited for slow response bodies to finish");
        if (cancelled.load() != 6)
            throw std::runtime_error("Not all transfers observed cancellation before shutdown returned");
        const int count = callbacks.load();
        Slic3r::Http::get(url + "/late").on_progress([&](Slic3r::Http::Progress, bool&) { ++callbacks; }).perform();
        // A rejected upload still owns its unopened request body until destruction.
        Slic3r::Http::put(url + "/late-put")
            .set_put_body(boost::filesystem::path(argv[2]))
            .on_progress([&](Slic3r::Http::Progress, bool&) { ++callbacks; })
            .perform();
        Slic3r::Http::shutdown_async(); // Repeated shutdown is harmless.
        if (callbacks.load() != count)
            throw std::runtime_error("Request accepted after shutdown");
        requests.clear();
        std::cout << "PASS HTTPS shutdown: chained requests, six cancelled transfers, no late request\n";
        return 0;
    } catch (const std::exception& ex) {
        Slic3r::Http::shutdown_async();
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
