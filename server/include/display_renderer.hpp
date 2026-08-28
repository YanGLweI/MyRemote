#pragma once

#include <QWidget>

#include <mutex>

// Shows the remote desktop and captures local input, mapping widget
// coordinates to the remote resolution.
class DisplayRenderer : public QWidget {
    Q_OBJECT

public:
    explicit DisplayRenderer(QWidget* parent = nullptr);

    QSize sizeHint() const override { return QSize(960, 540); }
    void set_remote_size(int width, int height);

public slots:
    // Stores the newest frame; safe to call from the decode thread, so it must
    // not touch QWidget. The pipeline's frame_ready signal drives repaint().
    void set_frame(QImage frame);
    void repaint_frame();
    void clear_frame();

signals:
    void mouse_moved(int x, int y);       // remote pixel coords
    void mouse_button_changed(int button, bool pressed);
    void mouse_wheelled(int delta);
    void key_changed(int vk, bool pressed, bool extended);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    QPoint map_to_remote(const QPoint& widget_pos) const;
    QRect remote_rect() const;  // letterbox area within the widget

    mutable std::mutex mutex_;
    QImage current_;
    int remote_w_ = 0;
    int remote_h_ = 0;
};
