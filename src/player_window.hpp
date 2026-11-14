#pragma once
#include "playback_controller.hpp"
#include <QRect>
#include <QMainWindow>
#include <QPoint>
#include <QElapsedTimer>
class QLabel;
class QSlider;
class QPushButton;
class QCloseEvent;
class QResizeEvent;
class QKeyEvent;
class QTimer;
class QGridLayout;
class QProgressBar;

namespace intezam {
class PlayerWindow final : public QMainWindow {
public:
    PlayerWindow(PlaybackControls& controls, const QString& title, std::int64_t video_size);
    std::uint32_t videoWindow();
protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void refresh();
    void toggleFullscreen();
    void leaveFullscreen();
    void revealControls();
    void hideInactiveControls();
    bool handleKey(QKeyEvent* event);
    void seekTo(std::int64_t milliseconds);
    void resizeDock();
    void updateTitle();
    void createLoadingView(QWidget* canvas);
    void refreshLoading(const PlaybackSnapshot& snapshot);
    PlaybackControls& controls_;
    const QString title_;
    QWidget* header_ = nullptr;
    QWidget* dock_ = nullptr;
    QWidget* audio_ = nullptr;
    QWidget* surface_ = nullptr;
    QWidget* loading_ = nullptr;
    QLabel* loading_title_ = nullptr;
    QLabel* loading_detail_ = nullptr;
    QLabel* loading_hint_ = nullptr;
    QLabel* loading_elapsed_text_ = nullptr;
    QProgressBar* loading_progress_ = nullptr;
    QPushButton* cancel_startup_ = nullptr;
    QElapsedTimer loading_elapsed_;
    QLabel* heading_ = nullptr;
    QLabel* state_ = nullptr;
    QLabel* time_ = nullptr;
    QLabel* notice_ = nullptr;
    QSlider* seek_ = nullptr;
    QSlider* volume_ = nullptr;
    QLabel* volume_text_ = nullptr;
    QPushButton* play_ = nullptr;
    QPushButton* back_ = nullptr;
    QPushButton* forward_ = nullptr;
    QPushButton* mute_ = nullptr;
    QPushButton* fullscreen_ = nullptr;
    QGridLayout* dock_layout_ = nullptr;
    QTimer* hide_timer_ = nullptr;
    QRect normal_geometry_;
    QPoint last_cursor_;
    bool was_maximized_ = false;
    bool keyboard_navigation_ = false;
    bool compact_dock_ = false;
    bool may_hide_ = false;
    bool closing_ = false;
    bool first_frame_seen_ = false;
};
void runQtPlayback(const std::string& magnet,
    std::shared_ptr<const libtorrent::torrent_info> metadata,
    const VideoFile& video, const PlaybackOptions& options = {});
}
