#include "player_window.hpp"
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTest>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
class FakeControls : public intezam::PlaybackControls {
public:
    std::vector<intezam::PlaybackCommand> commands;
    intezam::PlaybackSnapshot current;
    bool stopped = false;
    void submit(intezam::PlaybackCommand command) override { commands.push_back(command); }
    void requestStop() override { stopped = true; }
    intezam::PlaybackSnapshot snapshot() const override { return current; }
};
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try {
        FakeControls controls;
        intezam::PlayerWindow window(controls, "Test video", 60879760);
        window.show();
        QTest::qWait(30);
        require(!window.findChild<QPushButton*>("playPause")->isEnabled(), "controls disabled during preparation");
        controls.current.ready = true;
        controls.current.phase.clear();
        controls.current.player.state = libvlc_Playing;
        controls.current.player.seekable = true;
        controls.current.player.duration_ms = 100000;
        controls.current.player.time_ms = 50000;
        controls.current.source.cache.budget_bytes = 8400896;
        controls.current.source.waiting_reads = 1;
        controls.current.source.cache.network_bytes_per_second = 99 * 1048576;
        controls.current.source.cache.download_bytes_per_second = 1048576;
        QTest::qWait(250);
        require(window.findChild<QLabel*>("playbackState")->text() == "PLAYING",
            "prefetch read waits do not report playback stalls");
        require(window.findChild<QLabel*>("networkStatus")->text().contains("verified ahead"),
            "readout distinguishes verified disk data from decoder buffers");
        require(window.findChild<QLabel*>("networkStatus")->text().contains("1.00 MiB/s"),
            "download readout uses current transfer rate rather than a stale link estimate");
        auto* seek = window.findChild<QSlider*>("seek");
        require(!seek->hasTracking() && seek->isEnabled(), "seek on release enabled when seekable");
        const QPoint middle(seek->width() / 2, seek->height() / 2);
        QTest::mousePress(seek, Qt::LeftButton, Qt::NoModifier, middle);
        QTest::mouseMove(seek, QPoint(seek->width()*3/4, middle.y()));
        require(controls.commands.empty(), "dragging slider sends no seek");
        QTest::mouseRelease(seek, Qt::LeftButton, Qt::NoModifier, QPoint(seek->width()*3/4, middle.y()));
        require(controls.commands.size() == 1 && controls.commands.back().type == intezam::PlaybackCommandType::seek,
            "release sends exactly one seek");
        controls.commands.clear();
        window.activateWindow();
        QTest::qWait(30);
        QTest::keyClick(&window, Qt::Key_Space);
        require(controls.commands.size() == 1 && controls.commands[0].type == intezam::PlaybackCommandType::toggle_pause, "pause button");
        window.findChild<QPushButton*>("forward")->click();
        require(controls.commands.back().value == 10000, "forward button");
        window.findChild<QPushButton*>("mute")->click();
        require(controls.commands.back().type == intezam::PlaybackCommandType::mute, "mute button");
        require(window.findChild<QLabel*>("cacheStatus")->text().contains("13.80%"), "effective relative budget visible");
        window.resize(420, 600);
        QTest::qWait(30);
        const auto* fullscreen = window.findChild<QPushButton*>("fullscreen");
        const auto* volume = window.findChild<QSlider*>("volume");
        require(fullscreen->geometry().right() <= window.width() && volume->width() >= 70,
            "narrow window keeps fullscreen and usable volume controls visible");
        window.close();
        require(controls.stopped && window.isVisible(), "close requests cancellation and keeps native surface alive");
        controls.current.finished = true;
        QTest::qWait(250);
        require(!window.isVisible(), "close completes after worker shutdown");
        std::cout << "Qt controls, seek-on-release and asynchronous close tests passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
