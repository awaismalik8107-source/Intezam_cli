#include "local_video_torrent.hpp"
#include "player_window.hpp"
#include "playback_preparation.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QScreen>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTest>
#include <X11/Xlib.h>
#include <iostream>

using test_support::require;
using namespace std::chrono_literals;
namespace {
void waitFor(const std::function<bool()>& complete, const char* message, int timeout = 15000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!complete()) {
        require(elapsed.elapsed() < timeout, message);
        QTest::qWait(20);
    }
}
void playsAndCloses(const std::filesystem::path& media, bool native)
{
    test_support::LocalVideoTorrent torrent(media);
    intezam::PlaybackOptions options;
    options.cache = torrent.options();
    intezam::MetadataOptions metadata_options;
    metadata_options.settings = test_support::loopbackSettings();
    metadata_options.timeout = 5s;
    const auto discovery = intezam::retrieveTorrentMetadataWithPeers(torrent.magnet(), metadata_options);
    require(!discovery.peers.empty(), "metadata discovery retains a playback bootstrap peer");
    options.cache.bootstrap_peers = discovery.peers;
    const auto plain_magnet = torrent.magnet().substr(0, torrent.magnet().find("&x.pe="));
    options.preparation = std::make_shared<intezam::PlaybackPreparation>(plain_magnet, discovery.metadata, torrent.video(), options.cache);
    const auto warmed = options.preparation->waitForSource();
    options.preparation->waitForHeaders(); // Simulate time spent choosing the already prepared candidate.
    options.vlc.headless = !native;
    intezam::PlaybackController controller(options);
    intezam::PlayerWindow window(controller, "Intezam controller integration", torrent.video().size_bytes);
    window.show();
    if (native) controller.setVideoWindow(window.videoWindow());
    // No tracker, DHT, LSD or magnet peer hints: only the metadata handoff can connect.
    controller.start(plain_magnet, discovery.metadata, torrent.video());
    QElapsedTimer startup_wait;
    startup_wait.start();
    bool reported_startup = false;
    waitFor([&] {
        const auto status = controller.snapshot();
        if (!reported_startup && startup_wait.elapsed() > 12000) {
            reported_startup = true;
            std::cerr << "Startup stalled: phase=" << status.phase << " ready=" << status.ready
                << " position=" << status.source.cache.playback_position << " wait=" << status.source.waiting_reads
                << " rate=" << status.source.estimated_playback_bytes_per_second
                << " ahead=" << status.source.cache.forward_buffer_seconds << " target=" << status.source.startup_buffer_target_bytes
                << " verified=" << status.source.cache.verified_pieces << " peers=" << status.source.cache.connected_peers
                << " header_only=" << status.source.cache.header_read_mode
                << " rebuilds=" << status.source.cache.torrent_rebuilds << '\n';
        }
        require(status.player.error.empty(), "Qt playback: " + status.player.error);
        require(status.source.cache.allocated_bytes <= status.source.cache.budget_bytes, "Qt physical cache bound");
        return status.player.decoded_video_frames > 0 && status.player.seekable;
    }, "first Qt video frame");
    if (native && !qEnvironmentVariableIsEmpty("INTEZAM_TEST_SCREENSHOT")) {
        QTest::qWait(300);
        const auto screenshot = qEnvironmentVariable("INTEZAM_TEST_SCREENSHOT");
        window.grab().save(screenshot);
        // Native child video may be omitted by QWidget::grab. Capture the X11
        // window separately, and keep both artifacts for honest visual review.
        window.screen()->grabWindow(window.winId()).save(screenshot + ".screen.png");
    }
    QTest::qWait(220); // Wait for UI's next snapshot refresh.
    std::cout << "Prepared-source selection-to-first-frame check completed in " << startup_wait.elapsed() << " ms.\n";
    require(std::distance(std::filesystem::directory_iterator(options.cache.root), std::filesystem::directory_iterator{}) == 1,
        "Qt playback reuses the prepared cache instead of opening a second session");
    require(warmed->status().cache.playback_position >= 0 && !warmed->status().cache.header_read_mode,
        "the warm source transitions from headers into playback lookahead");
    require(!window.findChild<QWidget*>("startupLoading")->isVisible(), "loading view ends on a real decoded frame");
    window.findChild<QPushButton*>("playPause")->click();
    waitFor([&] { return controller.snapshot().player.user_paused; }, "Qt pause command");
    window.findChild<QSlider*>("volume")->setValue(37);
    waitFor([&] { return controller.snapshot().player.volume == 37; }, "Qt volume command");
    controller.submit({intezam::PlaybackCommandType::adjust_volume, 5});
    controller.submit({intezam::PlaybackCommandType::adjust_volume, 5});
    waitFor([&] { return controller.snapshot().player.volume == 47; }, "relative volume commands preserve repeated key steps");
    window.findChild<QPushButton*>("mute")->click();
    waitFor([&] { return controller.snapshot().player.muted; }, "Qt mute command");
    controller.submit({intezam::PlaybackCommandType::seek, 1000});
    QTest::qWait(200);
    require(controller.snapshot().player.user_paused, "seeking preserves user pause");
    waitFor([&] {
        const auto player = controller.snapshot().player;
        return player.user_paused && player.time_ms >= 750 && player.time_ms <= 1500;
    }, "seek reaches requested time while paused");
    window.findChild<QPushButton*>("fullscreen")->click();
    require(window.isFullScreen(), "Qt fullscreen");
    window.showNormal();
    QTest::qWait(220);
    require(window.findChild<QWidget*>("playerHeader")->isVisible(), "window-manager fullscreen exit restores header");
    window.findChild<QPushButton*>("playPause")->click();
    waitFor([&] { return !controller.snapshot().player.user_paused; }, "Qt resume command");
    const auto measured = controller.snapshot();
    window.close();
    waitFor([&] { return controller.snapshot().finished && !window.isVisible(); }, "Qt asynchronous shutdown", 7000);
    require(controller.snapshot().player.error.empty(), "Qt playback error after stop");
    require(std::filesystem::is_empty(options.cache.root), "Qt cache cleanup");
    std::cout << (native ? "Native X11" : "Offscreen") << " Qt + torrent + VLC decoded "
        << measured.player.decoded_video_frames << " video and " << measured.player.decoded_audio_frames
        << " audio frames; peak snapshot allocation " << measured.source.cache.allocated_bytes << " / "
        << measured.source.cache.budget_bytes << " bytes.\n";
}
void cancelsWhilePreparing(const std::filesystem::path& media)
{
    test_support::LocalVideoTorrent torrent(media);
    torrent.seed.pause();
    intezam::PlaybackOptions options;
    options.cache = torrent.options();
    options.vlc.headless = true;
    intezam::PlaybackController controller(options);
    intezam::PlayerWindow window(controller, "Cancel preparation test", torrent.video().size_bytes);
    window.show();
    controller.start(torrent.magnet(), torrent.metadata(), torrent.video());
    waitFor([&] { return controller.snapshot().source.waiting_reads > 0; }, "blocked preparation read");
    QTest::qWait(220);
    require(window.findChild<QWidget*>("startupLoading")->isVisible(), "blocked startup retains its loading view");
    if (!qEnvironmentVariableIsEmpty("INTEZAM_TEST_LOADING_NATIVE_SCREENSHOT"))
        window.grab().save(qEnvironmentVariable("INTEZAM_TEST_LOADING_NATIVE_SCREENSHOT"));
    QElapsedTimer closing;
    closing.start();
    window.findChild<QPushButton*>("cancelStartup")->click();
    require(closing.elapsed() < 100, "close must not join worker on UI thread");
    waitFor([&] { return controller.snapshot().finished && !window.isVisible(); }, "cancel during preparation", 5000);
    require(controller.snapshot().player.error.empty(), "intentional cancellation is not playback error");
    require(std::filesystem::is_empty(options.cache.root), "cancelled cache cleanup");
    controller.stopAndWait(); // Idempotent final teardown after the event loop exits.
    require(controller.snapshot().finished, "final teardown leaves worker joined and stopped");
}
}
int main(int argc, char** argv)
{
    const bool native = argc > 2 && std::string(argv[2]) == "--native";
    if (native) { require(XInitThreads() != 0, "X11 threading"); qputenv("QT_QPA_PLATFORM", "xcb"); }
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    try {
        require(argc >= 2, "media fixture required");
        playsAndCloses(argv[1], native);
        cancelsWhilePreparing(argv[1]);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
