#include "local_video_torrent.hpp"
#include "player_window.hpp"
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
    options.vlc.headless = !native;
    intezam::PlaybackController controller(options);
    intezam::PlayerWindow window(controller, "Intezam controller integration", torrent.video().size_bytes);
    window.show();
    if (native) controller.setVideoWindow(window.videoWindow());
    controller.start(torrent.magnet(), torrent.metadata(), torrent.video());
    waitFor([&] {
        const auto status = controller.snapshot();
        require(status.player.error.empty(), "Qt playback: " + status.player.error);
        require(status.source.cache.allocated_bytes <= status.source.cache.budget_bytes, "Qt physical cache bound");
        return status.player.decoded_video_frames > 0 && status.player.seekable;
    }, "first Qt video frame");
    if (native && !qEnvironmentVariableIsEmpty("INTEZAM_TEST_SCREENSHOT")) {
        QTest::qWait(300);
        window.grab().save(qEnvironmentVariable("INTEZAM_TEST_SCREENSHOT"));
    }
    QTest::qWait(220); // Wait for UI's next snapshot refresh.
    window.findChild<QPushButton*>("playPause")->click();
    waitFor([&] { return controller.snapshot().player.user_paused; }, "Qt pause command");
    window.findChild<QSlider*>("volume")->setValue(37);
    waitFor([&] { return controller.snapshot().player.volume == 37; }, "Qt volume command");
    window.findChild<QPushButton*>("mute")->click();
    waitFor([&] { return controller.snapshot().player.muted; }, "Qt mute command");
    controller.submit({intezam::PlaybackCommandType::seek, 1000});
    QTest::qWait(200);
    require(controller.snapshot().player.user_paused, "seeking preserves user pause");
    window.findChild<QPushButton*>("fullscreen")->click();
    require(window.isFullScreen(), "Qt fullscreen");
    window.showNormal();
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
    QElapsedTimer closing;
    closing.start();
    window.close();
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
