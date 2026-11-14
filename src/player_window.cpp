#include "player_window.hpp"
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace intezam {
namespace {
QString timestamp(std::int64_t milliseconds)
{
    const auto seconds = std::max<std::int64_t>(0, milliseconds / 1000);
    return QString("%1:%2:%3").arg(seconds / 3600, 2, 10, QChar('0'))
        .arg(seconds / 60 % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
}
QPushButton* button(QWidget* parent, const char* name, const QString& text, QStyle::StandardPixmap icon)
{
    auto* control = new QPushButton(parent->style()->standardIcon(icon), text, parent);
    control->setObjectName(name);
    control->setAccessibleName(text);
    control->setMinimumHeight(36);
    return control;
}
}
PlayerWindow::PlayerWindow(PlaybackControls& controls, const QString& title, std::int64_t video_size)
    : controls_(controls), video_size_(video_size)
{
    setWindowTitle(title + " — Intezam");
    resize(1000, 700);
    setMinimumSize(400, 450);
    auto* content = new QWidget(this);
    setCentralWidget(content);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(10);
    auto* heading = new QLabel(title, content);
    heading->setWordWrap(true);
    heading->setTextFormat(Qt::PlainText);
    heading->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QFont title_font = heading->font();
    title_font.setBold(true);
    heading->setFont(title_font);
    layout->addWidget(heading);
    surface_ = new QWidget(content);
    surface_->setObjectName("videoSurface");
    surface_->setAttribute(Qt::WA_NativeWindow);
    surface_->setMinimumSize(320, 180);
    surface_->setAutoFillBackground(true);
    auto palette = surface_->palette();
    palette.setColor(QPalette::Window, Qt::black);
    surface_->setPalette(palette);
    surface_->setFocusPolicy(Qt::StrongFocus);
    surface_->setAccessibleName("Video display");
    layout->addWidget(surface_, 1);
    auto* progress = new QHBoxLayout;
    seek_ = new QSlider(Qt::Horizontal, content);
    seek_->setObjectName("seek");
    seek_->setAccessibleName("Playback position");
    seek_->setToolTip("Seek on release · Left/Right: 10 seconds");
    seek_->setRange(0, 10000);
    seek_->setTracking(false);
    seek_->setSingleStep(100);
    time_ = new QLabel(content);
    time_->setMinimumWidth(165);
    progress->addWidget(seek_, 1);
    progress->addWidget(time_);
    layout->addLayout(progress);
    auto* transport = new QHBoxLayout;
    play_ = button(content, "playPause", "Pause", QStyle::SP_MediaPause);
    back_ = button(content, "back", "−10 s", QStyle::SP_MediaSeekBackward);
    forward_ = button(content, "forward", "+10 s", QStyle::SP_MediaSeekForward);
    auto* stop = button(content, "stop", "Stop", QStyle::SP_MediaStop);
    auto* fullscreen = button(content, "fullscreen", "Fullscreen", QStyle::SP_TitleBarMaxButton);
    mute_ = button(content, "mute", "Mute", QStyle::SP_MediaVolume);
    volume_ = new QSlider(Qt::Horizontal, content);
    volume_->setObjectName("volume");
    volume_->setRange(0, 100);
    volume_->setValue(100);
    volume_->setMaximumWidth(110);
    volume_->setAccessibleName("Volume");
    volume_->setToolTip("Volume (0–100%)");
    for (auto* control : {back_, play_, forward_, stop}) transport->addWidget(control);
    transport->addStretch();
    layout->addLayout(transport);
    auto* audio = new QHBoxLayout;
    audio->addWidget(mute_);
    audio->addWidget(volume_);
    audio->addStretch();
    audio->addWidget(fullscreen);
    layout->addLayout(audio);
    state_ = new QLabel(content);
    state_->setObjectName("playbackState");
    buffer_ = new QProgressBar(content);
    buffer_->setAccessibleName("Forward buffer");
    buffer_->setRange(0, 1000);
    buffer_->setMaximumHeight(14);
    buffer_->setTextVisible(false);
    network_ = new QLabel(content);
    network_->setObjectName("networkStatus");
    network_->setToolTip("Verified disk data beyond VLC’s current byte reader, estimated using average bitrate. Decoder queues are separate.");
    cache_ = new QLabel(content);
    cache_->setObjectName("cacheStatus");
    cache_->setWordWrap(true);
    network_->setWordWrap(true);
    notice_ = new QLabel(content);
    notice_->setWordWrap(true);
    notice_->setTextFormat(Qt::PlainText);
    notice_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(state_);
    layout->addWidget(buffer_);
    layout->addWidget(network_);
    layout->addWidget(cache_);
    layout->addWidget(notice_);
    connect(play_, &QPushButton::clicked, this, [this] { controls_.submit({PlaybackCommandType::toggle_pause}); });
    connect(back_, &QPushButton::clicked, this, [this] { controls_.submit({PlaybackCommandType::skip, -10000}); });
    connect(forward_, &QPushButton::clicked, this, [this] { controls_.submit({PlaybackCommandType::skip, 10000}); });
    connect(mute_, &QPushButton::clicked, this, [this] { controls_.submit({PlaybackCommandType::mute}); });
    connect(stop, &QPushButton::clicked, this, &QWidget::close);
    connect(fullscreen, &QPushButton::clicked, this, &PlayerWindow::toggleFullscreen);
    connect(volume_, &QSlider::valueChanged, this, [this](int value) { controls_.submit({PlaybackCommandType::volume, value}); });
    connect(seek_, &QSlider::valueChanged, this, [this](int value) {
        const auto media = controls_.snapshot().player;
        if (media.seekable && media.duration_ms > 0) controls_.submit({PlaybackCommandType::seek, media.duration_ms * value / 10000});
    });
    addShortcuts();
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &PlayerWindow::refresh);
    timer->start(200);
    refresh();
}
std::uint32_t PlayerWindow::videoWindow() { return static_cast<std::uint32_t>(surface_->winId()); }
void PlayerWindow::addShortcuts()
{
    const auto shortcut = [this](const QKeySequence& sequence, const std::function<void()>& action) {
        auto* key = new QShortcut(sequence, this);
        connect(key, &QShortcut::activated, this, action);
    };
    shortcut(QKeySequence(Qt::Key_Space), [this] { play_->click(); });
    shortcut(QKeySequence(Qt::Key_Left), [this] { back_->click(); });
    shortcut(QKeySequence(Qt::Key_Right), [this] { forward_->click(); });
    shortcut(QKeySequence(Qt::Key_M), [this] { mute_->click(); });
    shortcut(QKeySequence(Qt::Key_F), [this] { toggleFullscreen(); });
    shortcut(QKeySequence(Qt::Key_F11), [this] { toggleFullscreen(); });
    shortcut(QKeySequence(Qt::Key_Escape), [this] { if (isFullScreen()) showNormal(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Q), [this] { close(); });
}
void PlayerWindow::toggleFullscreen() { isFullScreen() ? showNormal() : showFullScreen(); }
void PlayerWindow::closeEvent(QCloseEvent* event)
{
    if (controls_.snapshot().finished) { event->accept(); return; }
    if (!closing_) { closing_ = true; controls_.requestStop(); }
    state_->setText("Stopping playback…");
    event->ignore(); // Keep the native output window alive until LibVLC releases it.
}
void PlayerWindow::refresh()
{
    const auto snapshot = controls_.snapshot();
    if (closing_ && snapshot.finished) { close(); return; }
    const auto& media = snapshot.player;
    const auto& cache = snapshot.source.cache;
    const bool enabled = snapshot.ready && !snapshot.finished && !closing_;
    for (auto* control : {play_, mute_}) control->setEnabled(enabled);
    volume_->setEnabled(enabled);
    const bool seekable = enabled && media.seekable && media.duration_ms > 0;
    for (auto* control : {back_, forward_}) control->setEnabled(seekable);
    seek_->setEnabled(seekable);
    if (!seek_->isSliderDown()) {
        const QSignalBlocker blocker(seek_);
        seek_->setValue(media.duration_ms > 0 ? static_cast<int>(media.time_ms * 10000 / media.duration_ms) : 0);
    }
    if (!volume_->isSliderDown()) { const QSignalBlocker blocker(volume_); volume_->setValue(media.volume); }
    play_->setText(media.user_paused ? "Play" : "Pause");
    play_->setAccessibleName(play_->text());
    play_->setIcon(style()->standardIcon(media.user_paused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
    mute_->setText(media.muted ? "Unmute" : "Mute");
    mute_->setAccessibleName(mute_->text());
    mute_->setIcon(style()->standardIcon(media.muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    time_->setText(timestamp(media.time_ms) + " / " + timestamp(media.duration_ms));
    QString state = QString::fromStdString(snapshot.phase);
    if (state.isEmpty()) state = media.user_paused ? "Paused" : media.waiting_for_buffer || media.state == libvlc_Buffering
        ? "Buffering…" : QString::fromLatin1(VlcPlayer::stateName(media.state));
    if (closing_) state = "Stopping playback…";
    if (!media.error.empty()) state = "Playback error";
    state_->setText(state);
    notice_->setText(QString::fromStdString(media.error.empty() ? snapshot.notice : media.error));
    notice_->setVisible(!notice_->text().isEmpty());
    network_->setText(QString("Download: %1 MiB/s   ·   ~%2 s verified ahead")
        .arg(cache.download_bytes_per_second / 1048576.0, 0, 'f', 2).arg(cache.forward_buffer_seconds, 0, 'f', 1));
    cache_->setText(QString("Cache: %1 / %2 MiB   ·   Budget: %3% of video (target 2.50%)")
        .arg(cache.allocated_bytes / 1048576.0, 0, 'f', 2).arg(cache.budget_bytes / 1048576.0, 0, 'f', 2)
        .arg(video_size_ > 0 ? cache.budget_bytes * 100.0 / video_size_ : 0, 0, 'f', 2));
    const auto targets = snapshot.source.buffer_targets;
    buffer_->setValue(static_cast<int>(std::clamp(cache.forward_buffer_seconds / std::max(0.001, targets.high_seconds), 0.0, 1.0) * 1000));
}
}
