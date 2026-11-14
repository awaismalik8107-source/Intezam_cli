#pragma once
#include "playback_controller.hpp"
#include <QMainWindow>
class QLabel;
class QSlider;
class QPushButton;
class QProgressBar;
class QCloseEvent;

namespace intezam {
class PlayerWindow final : public QMainWindow {
public:
    PlayerWindow(PlaybackControls& controls, const QString& title, std::int64_t video_size);
    std::uint32_t videoWindow();
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    void refresh();
    void toggleFullscreen();
    void addShortcuts();
    PlaybackControls& controls_;
    const std::int64_t video_size_;
    QWidget* surface_;
    QLabel* state_;
    QLabel* time_;
    QLabel* network_;
    QLabel* cache_;
    QLabel* notice_;
    QSlider* seek_;
    QSlider* volume_;
    QPushButton* play_;
    QPushButton* back_;
    QPushButton* forward_;
    QPushButton* mute_;
    QProgressBar* buffer_;
    bool closing_ = false;
};
void runQtPlayback(const std::string& magnet,
    std::shared_ptr<const libtorrent::torrent_info> metadata,
    const VideoFile& video, const PlaybackOptions& options = {});
}
