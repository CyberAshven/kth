// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// kth-wallet: wallet support answered from peers, with the local Electrum
// listener for wallets that do not speak P2P. It keeps headers and filters
// and downloads whole blocks where they match; it names nothing it watches to
// any peer. Prints "electrum <port>" once listening.
//
//   kth-wallet --network regtest --directory DIR --peer HOST:PORT [--peer ...]
//              [--electrum ADDRESS:PORT] [--birthday HEIGHT] [--sent-log FILE]
//              [--isolate] [--decoys N] [--download-all] [--scan-threads N]
//              [--no-input-prefix-filters]

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <asio/use_future.hpp>

#include <kth/node/wallet/electrum.hpp>
#include <kth/node/wallet/service.hpp>

namespace {

using kth::domain::config::network;

std::optional<network> network_of(std::string_view name) {
    if (name == "mainnet") return network::mainnet;
    if (name == "testnet") return network::testnet;
    if (name == "regtest") return network::regtest;
#if defined(KTH_CURRENCY_BCH)
    if (name == "testnet4") return network::testnet4;
    if (name == "scalenet") return network::scalenet;
    if (name == "chipnet") return network::chipnet;
#endif
    return std::nullopt;
}

int usage() {
    std::fprintf(stderr, "usage: kth-wallet --network NAME --directory DIR --peer HOST:PORT [--peer ...] "
        "[--electrum ADDRESS:PORT] [--birthday HEIGHT] [--sent-log FILE] [--isolate] [--decoys N] "
        "[--download-all] [--scan-threads N] [--no-input-prefix-filters]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    namespace wallet = kth::node::wallet;
    wallet::peer_settings settings;
    std::string electrum = "127.0.0.1:0";
    std::vector<std::string_view> args(argv + 1, argv + argc);
    for (size_t at = 0; at < args.size(); ++at) {
        auto const flag = args[at];
        auto const value = [&]() -> std::string {
            if (at + 1 >= args.size()) return {};
            return std::string(args[++at]);
        };
        if (flag == "--network") {
            auto const net = network_of(value());
            if ( ! net) return usage();
            settings.network = *net;
        } else if (flag == "--directory") {
            settings.directory = value();
        } else if (flag == "--peer") {
            settings.peers.push_back(value());
        } else if (flag == "--electrum") {
            electrum = value();
        } else if (flag == "--birthday") {
            settings.birthday = uint32_t(std::strtoul(value().c_str(), nullptr, 10));
        } else if (flag == "--sent-log") {
            settings.sent_log = value();
        } else if (flag == "--isolate") {
            settings.isolate_downloads = true;
        } else if (flag == "--decoys") {
            settings.decoy_blocks = size_t(std::strtoul(value().c_str(), nullptr, 10));
        } else if (flag == "--download-all") {
            settings.download_all_blocks = true;
        } else if (flag == "--scan-threads") {
            settings.scan_threads = size_t(std::strtoul(value().c_str(), nullptr, 10));
        } else if (flag == "--no-input-prefix-filters") {
            settings.input_prefix_filters = false;
        } else {
            return usage();
        }
    }
    if (settings.directory.empty() || settings.peers.empty()) return usage();
    auto const colon = electrum.rfind(':');
    if (colon == std::string::npos) return usage();
    auto const address = electrum.substr(0, colon);
    auto const port = uint16_t(std::strtoul(electrum.substr(colon + 1).c_str(), nullptr, 10));

    auto made = wallet::service::from_peers(settings);
    if ( ! made) {
        std::fprintf(stderr, "kth-wallet: cannot start: %s\n", made.error().message().c_str());
        return 1;
    }
    auto& service = **made;
    auto running = ::asio::co_spawn(service.executor(), service.run(), ::asio::use_future);
    {
        wallet::electrum_server listener(service, settings.network);
        auto const bound = listener.listen(address, port);
        if ( ! bound) {
            std::fprintf(stderr, "kth-wallet: cannot listen on %s: %s\n", electrum.c_str(), bound.error().message().c_str());
            service.stop();
            (void)running.wait_for(std::chrono::seconds(30));
            return 1;
        }
        ::asio::co_spawn(service.executor(), listener.run(), ::asio::detached);
        std::printf("electrum %u\n", unsigned(*bound));
        std::fflush(stdout);

        ::asio::io_context signals_context;
        ::asio::signal_set signals(signals_context, SIGINT, SIGTERM);
        signals.async_wait([](std::error_code, int) {});
        signals_context.run();
        listener.stop();
        service.stop();
        (void)running.wait_for(std::chrono::seconds(30));
    }
    return 0;
}
