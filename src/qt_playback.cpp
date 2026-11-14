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
    std::cout << "Qt playback controls opened. Space: pause; arrows: ±10s; M: mute; F: fullscreen.\n" << std::flush;
    app.exec();
    controller.stopAndWait(); // Also covers an external application quit bypassing closeEvent.
    const auto result = controller.snapshot();
    if (!result.player.error.empty()) throw std::runtime_error(result.player.error);
    std::cout << "Playback stopped. Temporary cache removed.\n";
}
}
