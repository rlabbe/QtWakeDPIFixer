#pragma once

#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QPointer>
#include <vector>

class QWidget;
class QTimer;

// Recovers window geometry after the machine resumes from sleep.
//
// On some Windows setups a resume leaves Qt's devicePixelRatio stale for a few
// seconds: the monitor comes back at 100% before Windows restores its real
// per-monitor scaling, so every window is mis-scaled until something forces a
// refresh. This watches the system suspend/resume power events, records each
// top-level window's geometry + devicePixelRatio at suspend, and once a
// window's live devicePixelRatio returns to its pre-sleep value, restores its
// geometry.
//
// Construct one instance after the QApplication. It covers every top-level
// window in the process; no inheritance or per-window code is required.
//
//     QApplication app(argc, argv);
//     WakeDpiFixer dpi_fixer;
class WakeDpiFixer : public QObject, public QAbstractNativeEventFilter {

public:
    explicit WakeDpiFixer(QObject* parent = nullptr);
    ~WakeDpiFixer() override;

    bool nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result) override;

private:
    struct SavedWindow {
        QPointer<QWidget> widget;
        QByteArray geometry;
        double dpr {1.0};
        bool restored {false};
    };

    void on_suspend();
    void on_resume();
    void poll();

    std::vector<SavedWindow> saved_;
    QTimer* poll_timer_ {nullptr};
    int poll_ticks_ {0};
};
