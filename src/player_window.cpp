#include "player_window.hpp"
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QProgressBar>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>

namespace intezam {
namespace {
QString timestamp(std::int64_t milliseconds)
{
    const auto seconds = std::max<std::int64_t>(0, milliseconds / 1000);
    if (seconds >= 3600) return QString("%1:%2:%3").arg(seconds / 3600)
        .arg(seconds / 60 % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}

enum class Glyph { play, pause, back, forward, stop, volume, muted, fullscreen, restore };
QIcon icon(Glyph glyph)
{
    QPixmap bitmap(48, 48);
    bitmap.setDevicePixelRatio(2);
    bitmap.fill(Qt::transparent);
    QPainter painter(&bitmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#eef0f5"), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const auto triangle = [&](bool right, int shift = 0) {
        const int a = right ? 8 + shift : 16 + shift, b = right ? 17 + shift : 7 + shift;
        painter.setBrush(QColor("#eef0f5"));
        painter.drawPolygon(QPolygonF{QPointF(a, 6), QPointF(b, 12), QPointF(a, 18)});
    };
    switch (glyph) {
        case Glyph::play: triangle(true); break;
        case Glyph::pause:
            painter.setBrush(QColor("#eef0f5"));
            painter.drawRoundedRect(QRectF(6, 5, 4, 14), 1, 1);
            painter.drawRoundedRect(QRectF(14, 5, 4, 14), 1, 1);
            break;
        case Glyph::back: triangle(false, 1); painter.drawLine(5, 6, 5, 18); break;
        case Glyph::forward: triangle(true, -1); painter.drawLine(19, 6, 19, 18); break;
        case Glyph::stop:
            painter.setBrush(QColor("#eef0f5"));
            painter.drawRoundedRect(QRectF(6, 6, 12, 12), 1, 1);
            break;
        case Glyph::volume:
        case Glyph::muted:
            painter.drawPolyline(QPolygonF{QPointF(4, 9), QPointF(8, 9), QPointF(13, 5), QPointF(13, 19), QPointF(8, 15), QPointF(4, 15), QPointF(4, 9)});
            if (glyph == Glyph::muted) { painter.drawLine(17, 9, 22, 15); painter.drawLine(22, 9, 17, 15); }
            else painter.drawArc(QRectF(12, 5, 9, 14), -60 * 16, 120 * 16);
            break;
        case Glyph::fullscreen:
            painter.drawPolyline(QPolygonF{QPointF(9, 5), QPointF(5, 5), QPointF(5, 9)});
            painter.drawPolyline(QPolygonF{QPointF(15, 5), QPointF(19, 5), QPointF(19, 9)});
            painter.drawPolyline(QPolygonF{QPointF(5, 15), QPointF(5, 19), QPointF(9, 19)});
            painter.drawPolyline(QPolygonF{QPointF(15, 19), QPointF(19, 19), QPointF(19, 15)});
            break;
        case Glyph::restore:
            painter.drawPolyline(QPolygonF{QPointF(5, 9), QPointF(9, 9), QPointF(9, 5)});
            painter.drawPolyline(QPolygonF{QPointF(15, 5), QPointF(15, 9), QPointF(19, 9)});
            painter.drawPolyline(QPolygonF{QPointF(5, 15), QPointF(9, 15), QPointF(9, 19)});
            painter.drawPolyline(QPolygonF{QPointF(15, 19), QPointF(15, 15), QPointF(19, 15)});
            break;
    }
    return QIcon(bitmap);
}

QPushButton* button(QWidget* parent, const char* name, const QString& label, Glyph glyph)
{
    auto* control = new QPushButton(icon(glyph), QString{}, parent);
    control->setObjectName(name);
    control->setAccessibleName(label);
    control->setToolTip(label);
    control->setFixedSize(36, 36);
    control->setIconSize({22, 22});
    control->setCursor(Qt::PointingHandCursor);
    return control;
}

// Absolute positioning replaces QSlider's page-step groove clicks. Tracking
// remains off: refresh cannot interrupt a drag, and only release commits it.
class PlayerSlider final : public QSlider {
public:
    explicit PlayerSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent)
    { setTracking(false); setMouseTracking(true); setRange(0, 10000); }
    std::int64_t duration = 0;
    std::function<void(int)> preview;
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor("#111216"));
        const QRectF track(6, height() / 2.0 - 2, std::max(1, width() - 12), 4);
        const auto fraction = double(sliderPosition() - minimum()) / std::max(1, maximum() - minimum());
        const auto x = track.left() + track.width() * fraction;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#424550"));
        painter.drawRoundedRect(track, 2, 2);
        painter.setBrush(QColor(isEnabled() ? "#a89bff" : "#5b5d67"));
        painter.drawRoundedRect(QRectF(track.left(), track.top(), x - track.left(), track.height()), 2, 2);
        if (hasFocus() && isEnabled()) painter.setPen(QPen(QColor("#b3a7ff"), 2));
        painter.setBrush(QColor(isEnabled() ? "#eef0f5" : "#5b5d67"));
        painter.drawEllipse(QPointF(x, track.center().y()), 5, 5);
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !isEnabled()) { QSlider::mousePressEvent(event); return; }
        setFocus(Qt::MouseFocusReason);
        setSliderDown(true);
        moveTo(event);
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (isSliderDown()) moveTo(event);
        else if (isEnabled() && duration > 0) QToolTip::showText(event->globalPosition().toPoint(),
            timestamp(duration * valueAt(event->position().toPoint()) / 10000), this);
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !isSliderDown()) { QSlider::mouseReleaseEvent(event); return; }
        moveTo(event);
        const auto target = sliderPosition();
        setSliderDown(false);
        setValue(target);
        event->accept();
    }
private:
    int valueAt(QPoint point) const
    {
        return QStyle::sliderValueFromPosition(minimum(), maximum(), point.x() - 6, std::max(1, width() - 12));
    }
    void moveTo(QMouseEvent* event)
    {
        setSliderPosition(valueAt(event->position().toPoint()));
        if (preview) preview(sliderPosition());
    }
};

const char* theme = R"(
QMainWindow, QWidget#playerContent { background: #08090b; color: #eef0f5; }
QWidget#playerHeader, QWidget#controlsDock { background: #111216; }
QWidget#controlsDock { border-top: 1px solid #282a32; }
QWidget#startupLoading { background: #08090b; }
QLabel#startupTitle { color: #eef0f5; font-size: 22px; font-weight: 600; }
QLabel#startupDetail { color: #ddd8f5; font-size: 14px; }
QLabel#startupHint, QLabel#startupElapsed { color: #aeb2bf; font-size: 12px; }
QProgressBar#startupProgress { border: none; border-radius: 3px; background: #282a35; }
QProgressBar#startupProgress::chunk { background: #a89bff; border-radius: 3px; }
QPushButton#cancelStartup { color: #eef0f5; background: #292b35; padding: 8px 20px; }
QPushButton#cancelStartup:hover { background: #3b3d49; }
QLabel { color: #aeb2bf; background: transparent; }
QLabel#videoTitle, QLabel#playbackTime { color: #e4e6ed; }
QLabel#brand { color: #a89bff; font-weight: 700; }
QLabel#playbackNotice { color: #ffbbb2; }
QPushButton { border: 1px solid transparent; border-radius: 8px; background: transparent; }
QPushButton:hover { background: #292b35; }
QPushButton:pressed { background: #3b3d49; }
QPushButton:focus { border: 2px solid #b3a7ff; }
QPushButton:disabled { background: transparent; }
QPushButton#playPause { background: #6552c8; border-radius: 22px; }
QPushButton#playPause:hover { background: #7c68df; }
QPushButton#playPause:disabled { background: #33303e; }
QSlider { background: transparent; }
QToolTip { background: #242631; color: #eef0f5; border: 1px solid #555866; padding: 5px; }
)";
}

PlayerWindow::PlayerWindow(PlaybackControls& controls, const QString& title, std::int64_t)
    : controls_(controls), title_(title)
{
    setWindowTitle(title + " — Intezam");
    resize(1000, 650);
    setMinimumSize(400, 320);
    setStyleSheet(QString::fromLatin1(theme));
    loading_elapsed_.start();
    auto* content = new QWidget(this);
    content->setObjectName("playerContent");
    setCentralWidget(content);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    header_ = new QWidget(content);
    header_->setObjectName("playerHeader");
    auto* header = new QHBoxLayout(header_);
    header->setContentsMargins(16, 9, 16, 9);
    auto* brand = new QLabel("INTEZAM", header_);
    brand->setObjectName("brand");
    header->addWidget(brand);
    heading_ = new QLabel(header_);
    heading_->setObjectName("videoTitle");
    heading_->setTextFormat(Qt::PlainText);
    heading_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    heading_->setToolTip(title);
    heading_->setAccessibleName(title);
    header->addWidget(heading_, 1);
    layout->addWidget(header_);
    auto* canvas = new QWidget(content);
    auto* canvas_layout = new QGridLayout(canvas);
    canvas_layout->setContentsMargins(0, 0, 0, 0);
    surface_ = new QWidget(canvas);
    surface_->setObjectName("videoSurface");
    surface_->setAttribute(Qt::WA_NativeWindow);
    surface_->setMinimumSize(320, 160);
    surface_->setAutoFillBackground(true);
    auto palette = surface_->palette();
    palette.setColor(QPalette::Window, Qt::black);
    surface_->setPalette(palette);
    surface_->setFocusPolicy(Qt::StrongFocus);
    surface_->setAccessibleName("Video display");
    canvas_layout->addWidget(surface_, 0, 0);
    createLoadingView(canvas);
    canvas_layout->addWidget(loading_, 0, 0);
    loading_->raise();
    layout->addWidget(canvas, 1);

    dock_ = new QWidget(content);
    dock_->setObjectName("controlsDock");
    dock_layout_ = new QGridLayout(dock_);
    dock_layout_->setContentsMargins(12, 4, 12, 10);
    dock_layout_->setHorizontalSpacing(12);
    dock_layout_->setVerticalSpacing(4);
    dock_layout_->setColumnStretch(1, 1);
    auto* timeline = new PlayerSlider(dock_);
    seek_ = timeline;
    seek_->setMinimumHeight(26);
    seek_->setObjectName("seek");
    seek_->setAccessibleName("Playback position");
    seek_->setAccessibleDescription("Click or drag to a time; release to seek. Left and right skip ten seconds.");
    dock_layout_->addWidget(seek_, 0, 0, 1, 3);
    auto* transport_widget = new QWidget(dock_);
    auto* transport = new QHBoxLayout(transport_widget);
    transport->setContentsMargins(0, 0, 0, 0);
    transport->setSpacing(6);
    back_ = button(transport_widget, "back", "Back 10 seconds (Left)", Glyph::back);
    play_ = button(transport_widget, "playPause", "Pause (Space)", Glyph::pause);
    play_->setFixedSize(44, 44);
    forward_ = button(transport_widget, "forward", "Forward 10 seconds (Right)", Glyph::forward);
    auto* stop = button(transport_widget, "stop", "Stop and close", Glyph::stop);
    for (auto* control : {back_, play_, forward_, stop}) transport->addWidget(control);
    dock_layout_->addWidget(transport_widget, 1, 0);
    time_ = new QLabel(dock_);
    time_->setObjectName("playbackTime");
    time_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    dock_layout_->addWidget(time_, 1, 1);
    audio_ = new QWidget(dock_);
    auto* audio = new QHBoxLayout(audio_);
    audio->setContentsMargins(0, 0, 0, 0);
    audio->setSpacing(6);
    mute_ = button(audio_, "mute", "Mute (M)", Glyph::volume);
    volume_ = new PlayerSlider(audio_);
    volume_->setObjectName("volume");
    volume_->setRange(0, 100);
    volume_->setTracking(true);
    volume_->setFixedWidth(80);
    volume_->setMinimumHeight(26);
    volume_->setAccessibleName("Volume");
    volume_text_ = new QLabel("100%", audio_);
    volume_text_->setMinimumWidth(38);
    fullscreen_ = button(audio_, "fullscreen", "Fullscreen (F / F11)", Glyph::fullscreen);
    audio->addWidget(mute_); audio->addWidget(volume_); audio->addWidget(volume_text_); audio->addWidget(fullscreen_);
    dock_layout_->addWidget(audio_, 1, 2);
    state_ = new QLabel(dock_);
    state_->setObjectName("playbackState");
    dock_layout_->addWidget(state_, 3, 0, 1, 3);
    notice_ = new QLabel(dock_);
    notice_->setObjectName("playbackNotice");
    notice_->setWordWrap(true);
    notice_->setTextFormat(Qt::PlainText);
    notice_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dock_layout_->addWidget(notice_, 4, 0, 1, 3);
    layout->addWidget(dock_);

    connect(play_, &QPushButton::clicked, this, [this] { controls_.submit({PlaybackCommandType::toggle_pause}); });
    connect(back_, &QPushButton::clicked, this, [this] { controls_.submit({PlaybackCommandType::skip, -10000}); });
    connect(forward_, &QPushButton::clicked, this, [this] { controls_.submit({PlaybackCommandType::skip, 10000}); });
    connect(mute_, &QPushButton::clicked, this, [this] { controls_.submit({PlaybackCommandType::mute}); });
    connect(stop, &QPushButton::clicked, this, &QWidget::close);
    connect(fullscreen_, &QPushButton::clicked, this, &PlayerWindow::toggleFullscreen);
    connect(volume_, &QSlider::valueChanged, this, [this](int value) { controls_.submit({PlaybackCommandType::volume, value}); });
    connect(seek_, &QSlider::valueChanged, this, [this](int value) {
        if (!seek_->isSliderDown()) {
            const auto media = controls_.snapshot().player;
            seekTo(media.duration_ms * value / 10000);
        }
    });
    timeline->preview = [this, timeline](int value) {
        time_->setText(timestamp(timeline->duration * value / 10000) + " / " + timestamp(timeline->duration));
    };
    hide_timer_ = new QTimer(this);
    hide_timer_->setObjectName("controlsHideTimer");
    hide_timer_->setSingleShot(true);
    hide_timer_->setInterval(3000);
    connect(hide_timer_, &QTimer::timeout, this, &PlayerWindow::hideInactiveControls);
    for (auto* widget : findChildren<QWidget*>()) { widget->setMouseTracking(true); widget->installEventFilter(this); }
    installEventFilter(this);
    last_cursor_ = QCursor::pos();
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &PlayerWindow::refresh);
    timer->start(200);
    resizeDock();
    refresh();
}
std::uint32_t PlayerWindow::videoWindow() { return static_cast<std::uint32_t>(surface_->winId()); }

void PlayerWindow::createLoadingView(QWidget* canvas)
{
    loading_ = new QWidget(canvas);
    loading_->setObjectName("startupLoading");
    // A separate opaque native sibling stays above LibVLC's X11 child until
    // its first decoded frame. The video surface stays mapped and unchanged.
    loading_->setAttribute(Qt::WA_NativeWindow);
    auto* layout = new QVBoxLayout(loading_);
    layout->setContentsMargins(24, 16, 24, 16);
    auto* card = new QWidget(loading_);
    card->setMinimumWidth(280);
    card->setMaximumWidth(500);
    auto* details = new QVBoxLayout(card);
    details->setContentsMargins(0, 0, 0, 0);
    details->setSpacing(14);
    loading_title_ = new QLabel(card);
    loading_title_->setObjectName("startupTitle");
    loading_detail_ = new QLabel(card);
    loading_detail_->setObjectName("startupDetail");
    loading_hint_ = new QLabel(card);
    loading_hint_->setObjectName("startupHint");
    loading_elapsed_text_ = new QLabel(card);
    loading_elapsed_text_->setObjectName("startupElapsed");
    for (auto* label : {loading_title_, loading_detail_, loading_hint_, loading_elapsed_text_}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setAlignment(Qt::AlignCenter);
    }
    loading_progress_ = new QProgressBar(card);
    loading_progress_->setObjectName("startupProgress");
    loading_progress_->setAccessibleName("Startup buffer progress");
    loading_progress_->setTextVisible(false);
    loading_progress_->setFixedHeight(6);
    cancel_startup_ = new QPushButton("Cancel", card);
    cancel_startup_->setObjectName("cancelStartup");
    cancel_startup_->setAccessibleName("Cancel loading and close");
    cancel_startup_->setMinimumHeight(36);
    connect(cancel_startup_, &QPushButton::clicked, this, &QWidget::close);
    details->addWidget(loading_title_);
    details->addWidget(loading_detail_);
    details->addWidget(loading_progress_);
    details->addWidget(loading_hint_);
    details->addWidget(loading_elapsed_text_);
    details->addWidget(cancel_startup_, 0, Qt::AlignHCenter);
    layout->addStretch();
    layout->addWidget(card, 0, Qt::AlignHCenter);
    layout->addStretch();
}

void PlayerWindow::refreshLoading(const PlaybackSnapshot& snapshot)
{
    first_frame_seen_ = first_frame_seen_ || snapshot.player.decoded_video_frames > 0;
    loading_->setVisible(!first_frame_seen_);
    if (first_frame_seen_) return;
    loading_->raise();
    loading_elapsed_text_->setText(timestamp(loading_elapsed_.elapsed()) + " elapsed");
    cancel_startup_->setEnabled(!closing_);
    const auto error = snapshot.player.error.empty() ? snapshot.source.error : snapshot.player.error;
    loading_progress_->setVisible(error.empty() && !snapshot.finished);
    cancel_startup_->setText(snapshot.finished || !error.empty() ? "Close" : "Cancel");
    cancel_startup_->setAccessibleName(snapshot.finished || !error.empty() ? "Close player" : "Cancel loading and close");
    if (!error.empty() || snapshot.finished) {
        loading_title_->setText(error.empty() ? "Playback stopped" : "Unable to start video");
        loading_detail_->setText(QString::fromStdString(error));
        loading_hint_->clear();
        return;
    }
    if (snapshot.startup.stage != StartupStage::buffering || snapshot.startup.target_seconds <= 0)
        loading_progress_->setRange(0, 0);
    loading_hint_->setText("Playback begins when the opening data is ready.");
    loading_detail_->setText("Waiting for the opening video data.");
    loading_title_->setText("Connecting to video");
    switch (snapshot.startup.stage) {
        case StartupStage::connecting: break;
        case StartupStage::reading_container:
            loading_title_->setText("Reading video headers");
            break;
        case StartupStage::buffering: {
            loading_title_->setText("Loading startup buffer");
            const auto target = snapshot.startup.target_seconds;
            const auto buffered = std::max(0.0, snapshot.startup.buffered_seconds);
            if (target > 0) {
                const int percent = static_cast<int>(std::clamp(buffered / target * 100, 0.0, 100.0));
                loading_progress_->setRange(0, 100);
                loading_progress_->setValue(percent);
                loading_detail_->setText(QString("%1% of startup buffer · ~%2 / %3 seconds")
                    .arg(percent).arg(std::min(buffered, target), 0, 'f', 1).arg(target, 0, 'f', 1));
                loading_hint_->setText("Estimated buffer target adapts to the connection.");
            }
            break;
        }
        case StartupStage::opening_video:
        case StartupStage::complete:
            loading_title_->setText("Opening video");
            loading_detail_->setText("Startup buffer ready. Waiting for the first frame.");
            break;
    }
    if (closing_) {
        loading_title_->setText("Stopping playback…");
        loading_detail_->setText("Closing the stream and removing its temporary cache.");
        loading_hint_->clear();
    }
}

void PlayerWindow::seekTo(std::int64_t milliseconds)
{
    const auto snapshot = controls_.snapshot();
    const auto& media = snapshot.player;
    if (!closing_ && snapshot.ready && !snapshot.finished && media.seekable && media.duration_ms > 0)
        controls_.submit({PlaybackCommandType::seek, std::clamp<std::int64_t>(milliseconds, 0, media.duration_ms - 1)});
}

bool PlayerWindow::handleKey(QKeyEvent* event)
{
    const auto key = event->key();
    const auto modifiers = event->modifiers();
    if (modifiers == Qt::ControlModifier && key == Qt::Key_Q) { close(); return true; }
    if (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
    if (event->isAutoRepeat() && (key == Qt::Key_Space || key == Qt::Key_M || key == Qt::Key_F || key == Qt::Key_F11)) return true;
    switch (key) {
        case Qt::Key_Space: if (play_->isEnabled()) play_->click(); return true;
        case Qt::Key_Left:
        case Qt::Key_Right:
            if (seek_->isEnabled()) controls_.submit({PlaybackCommandType::skip,
                (key == Qt::Key_Left ? -1 : 1) * (modifiers.testFlag(Qt::ShiftModifier) ? 30000 : 10000)});
            return true;
        case Qt::Key_Up:
        case Qt::Key_Down:
            if (volume_->isEnabled()) controls_.submit({PlaybackCommandType::adjust_volume, key == Qt::Key_Up ? 5 : -5});
            return true;
        case Qt::Key_M: if (mute_->isEnabled()) mute_->click(); return true;
        case Qt::Key_F:
        case Qt::Key_F11: toggleFullscreen(); return true;
        case Qt::Key_Escape: if (isFullScreen()) leaveFullscreen(); return true;
        case Qt::Key_Home: seekTo(0); return true;
        case Qt::Key_End: seekTo(controls_.snapshot().player.duration_ms - 1); return true;
        default: return false;
    }
}

bool PlayerWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == this && event->type() == QEvent::WindowStateChange && header_ && hide_timer_) {
        const bool fullscreen = isFullScreen();
        header_->setVisible(!fullscreen);
        fullscreen_->setIcon(icon(fullscreen ? Glyph::restore : Glyph::fullscreen));
        fullscreen_->setAccessibleName(fullscreen ? "Exit fullscreen" : "Fullscreen");
        fullscreen_->setToolTip(fullscreen ? "Exit fullscreen (Escape / F / F11)" : "Fullscreen (F / F11)");
        revealControls();
        if (!fullscreen) hide_timer_->stop();
    }
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress) {
        keyboard_navigation_ = false;
        revealControls();
        if (watched == surface_ && event->type() == QEvent::MouseButtonPress) surface_->setFocus(Qt::MouseFocusReason);
    }
    if (watched == surface_ && event->type() == QEvent::MouseButtonDblClick) { toggleFullscreen(); return true; }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) keyboard_navigation_ = true;
        revealControls();
        if (handleKey(key)) return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void PlayerWindow::toggleFullscreen()
{
    if (isFullScreen()) { leaveFullscreen(); return; }
    normal_geometry_ = geometry();
    was_maximized_ = isMaximized();
    showFullScreen();
    revealControls();
}

void PlayerWindow::leaveFullscreen()
{
    showNormal();
    if (was_maximized_) showMaximized();
    header_->show();
    dock_->show();
    if (!was_maximized_ && normal_geometry_.isValid()) setGeometry(normal_geometry_);
    hide_timer_->stop();
}

void PlayerWindow::revealControls()
{
    if (!dock_) return;
    dock_->show();
    if (isFullScreen() && may_hide_) hide_timer_->start();
}

void PlayerWindow::hideInactiveControls()
{
    if (!isFullScreen() || !may_hide_) return;
    if (seek_->isSliderDown() || volume_->isSliderDown() || QApplication::mouseButtons() != Qt::NoButton
        || dock_->rect().contains(dock_->mapFromGlobal(QCursor::pos()))
        || (keyboard_navigation_ && dock_->isAncestorOf(QApplication::focusWidget()))) {
        hide_timer_->start();
        return;
    }
    dock_->hide();
}

void PlayerWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (dock_layout_ && audio_) resizeDock();
    if (heading_) updateTitle();
}

void PlayerWindow::resizeDock()
{
    const bool compact = width() < 660;
    if (compact != compact_dock_) {
        compact_dock_ = compact;
        dock_layout_->removeWidget(audio_);
        dock_layout_->addWidget(audio_, compact ? 2 : 1, compact ? 0 : 2, 1, compact ? 3 : 1, Qt::AlignRight);
    }
}

void PlayerWindow::updateTitle()
{ heading_->setText(heading_->fontMetrics().elidedText(title_, Qt::ElideMiddle, std::max(0, heading_->width()))); }

void PlayerWindow::closeEvent(QCloseEvent* event)
{
    if (controls_.snapshot().finished) { event->accept(); return; }
    if (!closing_) { closing_ = true; controls_.requestStop(); }
    dock_->setEnabled(false);
    dock_->show();
    state_->show();
    state_->setText("Stopping playback…");
    event->ignore();
}

void PlayerWindow::refresh()
{
    const auto snapshot = controls_.snapshot();
    if (closing_ && snapshot.finished) { close(); return; }
    const auto& media = snapshot.player;
    refreshLoading(snapshot);
    const bool enabled = snapshot.ready && !snapshot.finished && !closing_;
    for (auto* control : {play_, mute_}) control->setEnabled(enabled);
    volume_->setEnabled(enabled);
    const bool seekable = enabled && media.seekable && media.duration_ms > 0;
    for (auto* control : {back_, forward_}) control->setEnabled(seekable);
    seek_->setEnabled(seekable);
    static_cast<PlayerSlider*>(seek_)->duration = media.duration_ms;
    seek_->setAccessibleDescription("Position " + timestamp(media.time_ms) + " of " + timestamp(media.duration_ms)
        + ". Click or drag to seek on release. Left and right skip ten seconds.");
    if (!seek_->isSliderDown()) {
        const QSignalBlocker blocker(seek_);
        seek_->setValue(media.duration_ms > 0 ? static_cast<int>(std::clamp<std::int64_t>(media.time_ms * 10000 / media.duration_ms, 0, 10000)) : 0);
        time_->setText(timestamp(media.time_ms) + " / " + (media.duration_ms > 0 ? timestamp(media.duration_ms) : "--:--"));
    }
    if (!volume_->isSliderDown()) { const QSignalBlocker blocker(volume_); volume_->setValue(media.volume); }
    volume_text_->setText(QString::number(media.volume) + '%');
    volume_->setToolTip(QString("Volume %1% (Up / Down)").arg(media.volume));
    const bool paused = media.user_paused || (media.state == libvlc_Paused && !media.waiting_for_buffer);
    play_->setIcon(icon(paused ? Glyph::play : Glyph::pause));
    play_->setAccessibleName(paused ? "Play" : "Pause");
    play_->setToolTip(paused ? "Play (Space)" : "Pause (Space)");
    mute_->setIcon(icon(media.muted || media.volume == 0 ? Glyph::muted : Glyph::volume));
    mute_->setAccessibleName(media.muted ? "Unmute" : "Mute");
    mute_->setToolTip(media.muted ? "Unmute (M)" : "Mute (M)");
    QString state = !snapshot.ready && !snapshot.finished ? "Loading video…" : media.user_paused ? "Paused"
        : media.waiting_for_buffer || media.state == libvlc_Buffering ? "Buffering…" : QString::fromLatin1(VlcPlayer::stateName(media.state));
    if (snapshot.finished) state = media.state == libvlc_Ended ? "Playback ended" : "Playback stopped";
    if (closing_) state = "Stopping playback…";
    if (!media.error.empty()) state = "Playback error";
    state_->setText(state);
    state_->setVisible(state != "PLAYING");
    notice_->setText(QString::fromStdString(media.error.empty() ? snapshot.notice : media.error));
    notice_->setVisible(!notice_->text().isEmpty());
    may_hide_ = first_frame_seen_ && enabled && media.state == libvlc_Playing && !paused && !media.waiting_for_buffer
        && media.error.empty() && snapshot.notice.empty();
    const auto cursor = QCursor::pos();
    if (isFullScreen() && cursor != last_cursor_ && rect().contains(mapFromGlobal(cursor))) revealControls();
    last_cursor_ = cursor;
    if (!isFullScreen() || !may_hide_) { dock_->show(); hide_timer_->stop(); }
    else if (dock_->isVisible() && !hide_timer_->isActive()) hide_timer_->start();
    updateTitle();
}
}
