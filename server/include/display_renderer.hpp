#pragma once

#include <QWidget>

#include <mutex>

// Shows the remote desktop. Receives decoded QImages on the GUI thread;
// paintEvent letterboxes the latest frame into the widget.
class DisplayRenderer : public QWidget {
    Q_OBJECT

public:
    explicit DisplayRenderer(QWidget* parent = nullptr);

    QSize sizeHint() const override { return QSize(960, 540); }
    bool has_frame() const;

public slots:
    void set_frame(const QImage& frame);
    void clear_frame();

signals:
    void fps_updated(float fps);
    // M5: input captured over the remote desktop area (widget coordinates).
    void mouse_event_captured(int x, int y, int button, bool pressed);
    void key_event_captured(int vk, bool pressed);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    mutable std::mutex mutex_;
    QImage current_;
};
