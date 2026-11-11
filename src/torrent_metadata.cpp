#include "torrent_metadata.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/disabled_disk_io.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_status.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lt = libtorrent;

namespace intezam {
namespace {

lt::add_torrent_params metadataTorrentParams(const std::string& magnet)
{
    lt::error_code error;
    auto params = lt::parse_magnet_uri(magnet, error);
    if (error) {
        throw std::invalid_argument("Invalid magnet link: " + error.message());
    }

    // Set these BEFORE adding the torrent: pausing after metadata_received_alert
    // alone races with the first payload requests. Auto-management can undo upload_mode.
    params.flags |= lt::torrent_flags::upload_mode | lt::torrent_flags::default_dont_download;
    params.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused
                      | lt::torrent_flags::share_mode | lt::torrent_flags::seed_mode
                      | lt::torrent_flags::stop_when_ready | lt::torrent_flags::sequential_download);
    // Ignore magnet so= priorities and web seeds; none may enable payload downloads.
    params.file_priorities.clear();
    params.piece_priorities.clear();
    params.url_seeds.clear();
    // Required by add_torrent, but never used for filesystem I/O by disabled_disk_io.
    params.save_path = ".";
    return params;
}

std::shared_ptr<const lt::torrent_info> waitForMetadata(
    lt::session& session, const lt::torrent_handle& torrent, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string last_failure;
    while (std::chrono::steady_clock::now() < deadline) {
        // Also check the handle in case the metadata alert was dropped from the queue.
        if (const auto metadata = torrent.torrent_file()) {
            return metadata;
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        session.wait_for_alert(std::min(remaining,
            std::chrono::steady_clock::duration{std::chrono::seconds{1}}));
        std::vector<lt::alert*> alerts;
        session.pop_alerts(&alerts);
        for (const auto* alert : alerts) {
            if (const auto* received = lt::alert_cast<lt::metadata_received_alert>(alert);
                received && received->handle == torrent) {
                if (const auto metadata = torrent.torrent_file()) {
                    return metadata;
                }
            }
            if (const auto* failed = lt::alert_cast<lt::metadata_failed_alert>(alert);
                failed && failed->handle == torrent) {
                // libtorrent retries corrupt metadata from another peer. Preserve
                // the reason in case no peer supplies valid metadata before timeout.
                last_failure = "Metadata failed: " + failed->error.message();
            }
            if (const auto* failed = lt::alert_cast<lt::torrent_error_alert>(alert);
                failed && failed->handle == torrent) {
                throw std::runtime_error("Torrent metadata error: " + failed->error.message());
            }
            if (const auto* failed = lt::alert_cast<lt::file_error_alert>(alert);
                failed && failed->handle == torrent) {
                throw std::runtime_error("Unexpected storage error: " + failed->error.message());
            }
            if (const auto* failed = lt::alert_cast<lt::session_error_alert>(alert)) {
                throw std::runtime_error("Torrent session error: " + failed->error.message());
            }
            if (lt::alert_cast<lt::tracker_error_alert>(alert)
                || lt::alert_cast<lt::listen_failed_alert>(alert)) {
                // A different tracker, DHT or outgoing connection may still work.
                last_failure = alert->message();
            }
        }
        const auto status = torrent.status();
        if (status.errc) {
            throw std::runtime_error("Torrent metadata error: " + status.errc.message());
        }
    }
    auto message = std::string{"Timed out waiting for torrent metadata. No reachable peer supplied metadata."};
    if (!last_failure.empty()) {
        message += " Last failure: " + last_failure;
    }
    throw std::runtime_error(message);
}

} // namespace

lt::settings_pack metadataSessionSettings()
{
    lt::settings_pack settings;
    settings.set_bool(lt::settings_pack::enable_dht, true);
    settings.set_bool(lt::settings_pack::enable_lsd, true);
    settings.set_bool(lt::settings_pack::enable_upnp, true);
    settings.set_bool(lt::settings_pack::enable_natpmp, true);
    settings.set_int(lt::settings_pack::stop_tracker_timeout, 1);
    return settings;
}

std::shared_ptr<const lt::torrent_info> retrieveTorrentMetadata(
    const std::string& magnet, const MetadataOptions& options)
{
    if (options.timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Metadata timeout must be positive.");
    }
    auto torrent_params = metadataTorrentParams(magnet);
    lt::session_params session_params(options.settings);
    session_params.settings.set_int(lt::settings_pack::alert_mask,
        static_cast<int>(static_cast<std::uint32_t>(
            lt::alert_category::status | lt::alert_category::error)));
    // Defense against ANY storage writes, including empty files, directories,
    // part files and accidental checks of existing files. This is not a cache.
    session_params.disk_io_constructor = lt::disabled_disk_io_constructor;
    lt::session session(std::move(session_params));
    lt::error_code error;
    const auto torrent = session.add_torrent(std::move(torrent_params), error);
    if (error) {
        throw std::runtime_error("Cannot add torrent for metadata: " + error.message());
    }
    const auto metadata = waitForMetadata(session, torrent, options.timeout);
    torrent.pause();
    session.remove_torrent(torrent);
    return metadata;
}

} // namespace intezam
