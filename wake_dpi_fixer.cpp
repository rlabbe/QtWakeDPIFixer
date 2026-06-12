#include "wake_dpi_fixer.h"

#include <QApplication>
#include <QDebug>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr int poll_interval_ms = 500;
constexpr int max_poll_ticks = 60;       // give up after ~30 s
constexpr int stable_ticks_required = 6; // need ~3 s of stable match before stopping
constexpr double dpr_epsilon = 0.01;
}


WakeDpiFixer::WakeDpiFixer(QObject* parent)
    : QObject(parent)
{
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(poll_interval_ms);
    connect(poll_timer_, &QTimer::timeout, this, &WakeDpiFixer::poll);

    qApp->installNativeEventFilter(this);
    qInfo() << "WakeDpiFixer: installed";
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
        if (msg->wParam == PBT_APMSUSPEND) {
            qInfo() << "WakeDpiFixer: WM_POWERBROADCAST PBT_APMSUSPEND";
            on_suspend();
        } else if (msg->wParam == PBT_APMRESUMEAUTOMATIC || msg->wParam == PBT_APMRESUMESUSPEND) {
            qInfo() << "WakeDpiFixer: WM_POWERBROADCAST resume wParam=" << msg->wParam;
            on_resume();
        }
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
        sw.rect = w->geometry();
        sw.dpr = w->devicePixelRatioF();
        saved_.push_back(sw);
        qInfo() << "WakeDpiFixer: suspend saved" << w << "dpr=" << sw.dpr << "geom=" << sw.rect
                << "screen=" << (w->screen() ? w->screen()->name() : QStringLiteral("?"));
    }
    qInfo() << "WakeDpiFixer: suspend total saved=" << saved_.size();
}


void WakeDpiFixer::on_resume()
{
    if (saved_.empty()) {
        qInfo() << "WakeDpiFixer: resume but nothing saved, ignoring";
        return;
    }
    poll_ticks_ = 0;
    poll_timer_->start();
    qInfo() << "WakeDpiFixer: resume, starting poll over" << saved_.size() << "windows";
}


// Polled after resume: each window's devicePixelRatio is stale until Windows
// finishes restoring the monitor. On every tick re-check dpr and geometry; if
// the dpr matches the pre-sleep value but the geometry has drifted, restore
// it. Do not stop on the first match: Windows can keep changing the display
// for a second or more after dpr first looks correct, so wait until the state
// has been stable for several consecutive ticks before declaring the recovery
// finished.
void WakeDpiFixer::poll()
{
    ++poll_ticks_;
    bool all_stable = true;

    for (SavedWindow& sw : saved_) {
        QWidget* w = sw.widget;
        if (!w) {
            sw.stable_ticks = stable_ticks_required; // window destroyed; treat as done
            continue;
        }

        double current = w->devicePixelRatioF();
        double diff = current - sw.dpr;
        if (diff < 0)
            diff = -diff;
        const bool dpr_ok = diff < dpr_epsilon;
        const bool geom_ok = w->geometry() == sw.rect;

        qDebug() << "WakeDpiFixer: poll tick" << poll_ticks_ << "win" << w << "current_dpr=" << current
                 << "saved_dpr=" << sw.dpr << "geom=" << w->geometry() << "expected=" << sw.rect
                 << "screen=" << (w->screen() ? w->screen()->name() : QStringLiteral("?"));

        if (dpr_ok && geom_ok) {
            sw.stable_ticks++;
        } else if (dpr_ok) {
            // Right dpi, wrong geometry — put the window back.
            w->restoreGeometry(sw.geometry);
            sw.stable_ticks = 0;
            qInfo() << "WakeDpiFixer: restored" << w << "at tick" << poll_ticks_ << "dpr=" << current
                    << "geom=" << w->geometry();
        } else {
            // dpi still wrong; geometry restore would be wrong too.
            sw.stable_ticks = 0;
        }

        if (sw.stable_ticks < stable_ticks_required)
            all_stable = false;
    }

    if (all_stable) {
        qInfo() << "WakeDpiFixer: all windows stable, polling stopped at tick" << poll_ticks_;
        poll_timer_->stop();
        saved_.clear();
    } else if (poll_ticks_ >= max_poll_ticks) {
        qInfo() << "WakeDpiFixer: timeout at tick" << poll_ticks_ << ", giving up";
        poll_timer_->stop();
        saved_.clear();
    }
}
