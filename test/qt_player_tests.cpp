#include "player_window.hpp"
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QSlider>
#include <QTest>
#include <QTimer>
#include <QToolTip>
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
        using Type = intezam::PlaybackCommandType;
        FakeControls controls;
        intezam::PlayerWindow window(controls, "A long example video title for accessible playback controls.mkv", 60879760);
        window.show();
        window.resize(960, 650);
        QTest::qWait(30);
        auto* play = window.findChild<QPushButton*>("playPause");
        auto* seek = window.findChild<QSlider*>("seek");
        auto* volume = window.findChild<QSlider*>("volume");
        auto* surface = window.findChild<QWidget*>("videoSurface");
        auto* dock = window.findChild<QWidget*>("controlsDock");
        require(!play->isEnabled(), "controls disabled during preparation");
        auto* loading = window.findChild<QWidget*>("startupLoading");
        auto* startup_progress = window.findChild<QProgressBar*>("startupProgress");
        require(loading && loading->isVisible() && startup_progress, "startup has a visible loading view");
        require(startup_progress->minimum() == 0 && startup_progress->maximum() == 0,
            "unknown connection progress is indeterminate");
        controls.current.startup.stage = intezam::StartupStage::buffering;
        controls.current.startup.buffered_seconds = 2;
        controls.current.startup.target_seconds = 8;
        QTest::qWait(220);
        require(startup_progress->value() == 25 && startup_progress->maximum() == 100,
            "loading progress reflects the startup buffer, not cache allocation");
        require(window.findChild<QLabel*>("startupDetail")->text().contains("25%"), "loading explains its measured percentage");
        if (!qEnvironmentVariableIsEmpty("INTEZAM_TEST_LOADING_SCREENSHOT"))
            window.grab().save(qEnvironmentVariable("INTEZAM_TEST_LOADING_SCREENSHOT"));
        controls.current.startup.stage = intezam::StartupStage::opening_video;
        controls.current.ready = true;
        QTest::qWait(220);
        require(loading->isVisible() && startup_progress->maximum() == 0,
            "loading continues through decoder startup instead of claiming the video has started at buffer completion");
        controls.current.player.error = "No peer supplied the opening video data.";
        QTest::qWait(220);
        require(loading->isVisible() && !startup_progress->isVisible()
            && window.findChild<QLabel*>("startupDetail")->text().contains("No peer"), "startup errors stay readable");
        controls.current.player.error.clear();
        require(!window.findChild<QLabel*>("networkStatus") && !window.findChild<QLabel*>("cacheStatus"),
            "technical diagnostics belong in the CLI");
        controls.current.ready = true;
        controls.current.phase.clear();
        controls.current.player.state = libvlc_Playing;
        controls.current.player.decoded_video_frames = 1;
        controls.current.player.seekable = true;
        controls.current.player.duration_ms = 100000;
        controls.current.player.time_ms = 50000;
        controls.current.player.volume = 65;
        controls.current.source.waiting_reads = 1;
        QTest::qWait(220);
        require(!loading->isVisible(), "first decoded frame dismisses loading");
        require(window.findChild<QLabel*>("playbackState")->text() == "PLAYING",
            "brief byte-reader waits do not claim a visible playback stall");
        require(surface->height() > window.height() * 0.65, "video occupies most of the window");
        require(!seek->hasTracking() && seek->isEnabled(), "seek on release when seekable");
        require(volume->value() == 65, "volume synchronized from authoritative snapshot");
        QTest::mouseMove(seek, QPoint(seek->width() / 4, seek->height() / 2));
        QTest::qWait(50);
        require(QToolTip::text().startsWith("0:2"), "hover previews the target playback time");
        QToolTip::hideText();

        // Groove click must target the chosen time, not move one page step.
        const QPoint target(seek->width() * 3 / 4, seek->height() / 2);
        QTest::mousePress(seek, Qt::LeftButton, Qt::NoModifier, target);
        require(controls.commands.empty(), "press sends no seek");
        QTest::qWait(230);
        require(seek->sliderPosition() > 7000, "snapshot refresh cannot interrupt a held timeline");
        require(window.findChild<QLabel*>("playbackTime")->text() != "0:50 / 1:40", "target time preview while holding");
        QTest::mouseRelease(seek, Qt::LeftButton, Qt::NoModifier, target);
        require(controls.commands.size() == 1 && controls.commands.back().type == Type::seek
            && controls.commands.back().value > 70000 && controls.commands.back().value < 80000,
            "groove click commits exactly one absolute seek on release");
        controls.commands.clear();
        QTest::mousePress(seek, Qt::LeftButton, Qt::NoModifier, QPoint(seek->width()/3, target.y()));
        QTest::mouseMove(seek, QPoint(seek->width()/5, target.y()));
        require(controls.commands.empty(), "drag sends no intermediate seeks");
        QTest::mouseRelease(seek, Qt::LeftButton, Qt::NoModifier, QPoint(seek->width()/5, target.y()));
        require(controls.commands.size() == 1 && controls.commands.back().value < 25000, "thumb drag commits once");
        controls.commands.clear();

        window.activateWindow();
        seek->setFocus();
        QTest::qWait(30);
        const auto key = [&](Qt::Key value, Qt::KeyboardModifiers modifiers, Type type, std::int64_t expected) {
            controls.commands.clear();
            QTest::keyClick(seek, value, modifiers);
            require(controls.commands.size() == 1 && controls.commands.back().type == type
                && controls.commands.back().value == expected, "focused-widget keyboard shortcut");
        };
        key(Qt::Key_Space, Qt::NoModifier, Type::toggle_pause, 0);
        key(Qt::Key_Left, Qt::NoModifier, Type::skip, -10000);
        key(Qt::Key_Right, Qt::NoModifier, Type::skip, 10000);
        key(Qt::Key_Left, Qt::ShiftModifier, Type::skip, -30000);
        key(Qt::Key_Right, Qt::ShiftModifier, Type::skip, 30000);
        key(Qt::Key_Up, Qt::NoModifier, Type::adjust_volume, 5);
        key(Qt::Key_Down, Qt::NoModifier, Type::adjust_volume, -5);
        key(Qt::Key_Home, Qt::NoModifier, Type::seek, 0);
        key(Qt::Key_End, Qt::NoModifier, Type::seek, 99999);
        key(Qt::Key_M, Qt::NoModifier, Type::mute, 0);
        controls.commands.clear();
        play->setFocus();
        QTest::keyClick(play, Qt::Key_Space);
        require(controls.commands.size() == 1 && controls.commands.back().type == Type::toggle_pause,
            "Space on a focused button does not double-toggle");
        controls.commands.clear();
        window.findChild<QPushButton*>("back")->click();
        window.findChild<QPushButton*>("forward")->click();
        require(controls.commands.size() == 2 && controls.commands[0].value == -10000 && controls.commands[1].value == 10000,
            "skip buttons dispatch backward and forward commands");

        controls.current.player.user_paused = true;
        controls.current.player.muted = true;
        QTest::qWait(220);
        require(play->accessibleName() == "Play", "pause state reflected by icon/accessibility");
        require(window.findChild<QPushButton*>("mute")->accessibleName() == "Unmute", "mute state synchronized");
        controls.commands.clear();
        seek->setValue(10000);
        require(controls.commands.size() == 1 && controls.commands.back().value == 99999,
            "end seek clamps below duration while paused");
        controls.current.player.seekable = false;
        QTest::qWait(220);
        require(!seek->isEnabled(), "nonseekable media disables timeline");
        controls.commands.clear();
        QTest::keyClick(play, Qt::Key_End);
        QTest::keyClick(play, Qt::Key_Right);
        require(controls.commands.empty(), "shortcuts honor seekability");
        controls.current.player.seekable = true;
        controls.current.player.user_paused = false;
        QTest::qWait(220);

        const auto native_id = window.videoWindow();
        const auto normal_size = window.size();
        QTest::keyClick(&window, Qt::Key_F);
        require(window.isFullScreen() && window.videoWindow() == native_id, "fullscreen preserves native output surface");
        surface->setFocus();
        QTest::mouseMove(surface, surface->rect().center());
        QTest::qWait(3300);
        require(!dock->isVisible(), "fullscreen dock hides after inactivity");
        QTest::mouseMove(surface, surface->rect().center() + QPoint(20, 0));
        QTest::qWait(250); // Native child motion is also detected by the snapshot timer.
        require(dock->isVisible(), "pointer movement reveals fullscreen controls");
        controls.current.player.user_paused = true;
        QTest::qWait(3300);
        require(dock->isVisible(), "paused playback keeps controls visible");
        QTest::keyClick(&window, Qt::Key_Escape);
        require(!window.isFullScreen() && window.videoWindow() == native_id, "Escape restores window without replacing video");
        require(window.size() == normal_size, "previous window size restored");
        QTest::keyClick(&window, Qt::Key_F11);
        require(window.isFullScreen(), "F11 fullscreen shortcut");
        QTest::keyClick(&window, Qt::Key_F11);
        require(!window.isFullScreen(), "F11 exits fullscreen");

        controls.current.player.user_paused = false;
        controls.current.player.waiting_for_buffer = true;
        QTest::qWait(220);
        require(window.findChild<QLabel*>("playbackState")->text().contains("Buffering"), "actual buffering is displayed");
        controls.current.player.error = "Unable to read the video. Please try again.";
        QTest::qWait(220);
        require(window.findChild<QLabel*>("playbackNotice")->isVisible(), "playback errors remain readable");
        controls.current.player.error.clear();
        window.resize(420, 600);
        QTest::qWait(220);
        const auto* fullscreen = window.findChild<QPushButton*>("fullscreen");
        require(fullscreen->mapTo(&window, QPoint(fullscreen->width(), 0)).x() <= window.width()
            && volume->width() >= 70, "narrow window retains usable fullscreen/audio controls");
        if (!qEnvironmentVariableIsEmpty("INTEZAM_TEST_SCREENSHOT"))
            window.grab().save(qEnvironmentVariable("INTEZAM_TEST_SCREENSHOT"));
        QTest::keyClick(&window, Qt::Key_Q, Qt::ControlModifier);
        require(controls.stopped && window.isVisible(), "Ctrl+Q cancels without destroying active VLC surface");
        controls.current.finished = true;
        QTest::qWait(220);
        require(!window.isVisible(), "close completes after worker shutdown");
        std::cout << "Native control, timeline, keyboard, fullscreen and asynchronous shutdown tests passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
