#pragma once
#include <QWidget>
#include <QCloseEvent>
#include <functional>

namespace ambilight {
class MirrorWindow final : public QWidget {
public:
    explicit MirrorWindow(QWidget* parent) : QWidget(parent,
        Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowStaysOnTopHint)
    {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_PaintOnScreen);
        setAttribute(Qt::WA_NoSystemBackground);
        setWindowTitle(QStringLiteral("Ambilight — TV mirror"));
    }
    std::function<void()> stopRequested;
    QPaintEngine* paintEngine() const override { return nullptr; }
protected:
    void paintEvent(QPaintEvent*) override {} // Direct3D owns the entire surface.
    void closeEvent(QCloseEvent* event) override {
        if (stopRequested) stopRequested();
        // The callback synchronously releases D3D ownership. Do not veto
        // QApplication::quit or a Windows close request afterwards.
        event->accept();
    }
};
}
