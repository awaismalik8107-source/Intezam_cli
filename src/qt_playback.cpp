#include "player_window.hpp"
#include <QApplication>
#include <QTimer>
#include <X11/Xlib.h>
#include <csignal>
#include <iostream>
#include <stdexcept>

namespace intezam {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interruptPlayback(int) { interrupted = 1; }
class PlaybackSignals {
public:
    PlaybackSignals() : previous_interrupt_(std::signal(SIGINT, interruptPlayback)),
        previous_terminate_(std::signal(SIGTERM, interruptPlayback)) { interrupted = 0; }
    ~PlaybackSignals() { std::signal(SIGINT, previous_interrupt_); std::signal(SIGTERM, previous_terminate_); }
private:
    using Handler = void (*)(int);
    Handler previous_interrupt_;
    Handler previous_terminate_;
};
}
void runQtPlayback(const std::string& magnet, std::shared_ptr<const libtorrent::torrent_info> metadata,
    const VideoFile& video, const PlaybackOptions& options)
{
    if (qEnvironmentVariableIsEmpty("DISPLAY")) {
        throw std::runtime_error("Qt/LibVLC 3 playback requires X11 or XWayland (DISPLAY). Use --no-ui or --headless.");
    }
    // LibVLC 3 embeds an X11 drawable. Xlib must be initialized before Qt opens a display.
    if (!XInitThreads()) throw std::runtime_error("Cannot initialize X11 threading for embedded VLC.");
    qputenv("QT_QPA_PLATFORM", "xcb");
    int argc = 1;
    char name[] = "intezam";
    char* argv[]{name, nullptr};
    QApplication app(argc, argv);
    app.setApplicationName("Intezam");
    PlaybackSignals signal_handlers;
    CliStatusRenderer monitor(std::cout, options.monitoring);
    // Window construction precedes player construction, but needs the controller interface.
    PlaybackController controller(options);
    PlayerWindow window(controller, QString::fromStdString(video.path), video.size_bytes);
    window.show();
    // Attach the native output before the worker can create LibVLC.
    controller.setVideoWindow(window.videoWindow());
    controller.start(magnet, std::move(metadata), video);
    QTimer signal_timer;
    QObject::connect(&signal_timer, &QTimer::timeout, &window, [&] { if (interrupted) window.close(); });
    signal_timer.start(100);
    bool started = false;
    std::string last_notice, last_error, last_phase;
    QTimer status_timer;
    QObject::connect(&status_timer, &QTimer::timeout, &window, [&] {
        const auto snapshot = controller.snapshot();
        if (!snapshot.phase.empty() && snapshot.phase != last_phase && !snapshot.finished)
            monitor.event("Loading: " + snapshot.phase);
        last_phase = snapshot.phase;
        if (!started && snapshot.player.decoded_video_frames > 0) {
            monitor.event("Playback started.");
            started = true;
        }
        if (!snapshot.notice.empty() && snapshot.notice != last_notice)
            monitor.event("Playback control: " + snapshot.notice);
        last_notice = snapshot.notice;
        if (!snapshot.player.error.empty() && snapshot.player.error != last_error)
            monitor.event("Playback failed: " + snapshot.player.error);
        last_error = snapshot.player.error;
        monitor.update(snapshot.player, snapshot.source, snapshot.phase);
    });
    status_timer.start(700);
    monitor.event("Native player opened. Space: pause; arrows: skip; Up/Down: volume; F: fullscreen.");
    app.exec();
    controller.stopAndWait(); // Also covers an external application quit bypassing closeEvent.
    const auto result = controller.snapshot();
    if (!result.player.error.empty()) {
        if (last_error != result.player.error) monitor.event("Playback failed: " + result.player.error);
        monitor.finish();
        throw std::runtime_error(result.player.error);
    }
    monitor.event("Playback stopped. Temporary cache removed.");
}
}
