#include "wake_dpi_fixer.h"

#include <QApplication>
#include <QTimer>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr int poll_interval_ms = 500;
constexpr int max_poll_ticks = 60; // give up after ~30 s
constexpr double dpr_epsilon = 0.01;
}


WakeDpiFixer::WakeDpiFixer(QObject* parent)
    : QObject(parent)
{
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(poll_interval_ms);
    connect(poll_timer_, &QTimer::timeout, this, &WakeDpiFixer::poll);

    qApp->installNativeEventFilter(this);
}


WakeDpiFixer::~WakeDpiFixer()
{
    if (qApp)
        qApp->removeNativeEventFilter(this);
}


bool WakeDpiFixer::nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result)
{
    Q_UNUSED(event_type);
    Q_UNUSED(result);

#ifdef Q_OS_WIN
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg && msg->message == WM_POWERBROADCAST) {
        if (msg->wParam == PBT_APMSUSPEND)
            on_suspend();
        else if (msg->wParam == PBT_APMRESUMEAUTOMATIC || msg->wParam == PBT_APMRESUMESUSPEND)
            on_resume();
    }
#else
    Q_UNUSED(message);
#endif
    return false; // observe only, never consume
}


void WakeDpiFixer::on_suspend()
{
    poll_timer_->stop();
    saved_.clear();

    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (!w->isWindow() || !w->isVisible())
            continue;
        SavedWindow sw;
        sw.widget = w;
        sw.geometry = w->saveGeometry();
        sw.dpr = w->devicePixelRatioF();
        saved_.push_back(sw);
    }
}


void WakeDpiFixer::on_resume()
{
    if (saved_.empty())
        return;
    poll_ticks_ = 0;
    poll_timer_->start();
}


// Polled after resume: each window's devicePixelRatio is stale until Windows
// finishes restoring the monitor. When it returns to the pre-sleep value the
// window is valid again, so restore the geometry recorded at suspend.
void WakeDpiFixer::poll()
{
    ++poll_ticks_;
    bool all_done = true;

    for (SavedWindow& sw : saved_) {
        if (sw.restored)
            continue;

        QWidget* w = sw.widget;
        if (!w) {
            sw.restored = true; // window destroyed while we waited
            continue;
        }

        double diff = w->devicePixelRatioF() - sw.dpr;
        if (diff < 0)
            diff = -diff;

        if (diff < dpr_epsilon) {
            w->restoreGeometry(sw.geometry);
            sw.restored = true;
        } else {
            all_done = false;
        }
    }

    // Stop once every window is restored, or give up after the timeout so a
    // monitor that never returns to its old scale does not poll forever.
    if (all_done || poll_ticks_ >= max_poll_ticks) {
        poll_timer_->stop();
        saved_.clear();
    }
}
