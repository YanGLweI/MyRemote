#pragma once

#include <QWidget>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QtGui/QPainter>

// Server-side display renderer - decodes and renders H.264 video frames
class DisplayRenderer : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
    
public:
    explicit DisplayRenderer(QWidget* parent = nullptr);
    ~DisplayRenderer() override;
    
    // Render new video frame
    void render_frame(const uint8_t* h264_data, size_t data_size);
    
    // Handle mouse events (forward to remote input)
    void handle_mouse_event(int x, int y, bool button_pressed, 
                           unsigned int button_flags);
    
    // Resize frame dimensions
    void set_frame_size(int width, int height);
    
    // Get current frame statistics
    float get_fps() const { return last_fps_; }
    
signals:
    void fps_changed(float fps);
    
private:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    
    // H.264 decoder helper (could use FFmpeg or libavcodec)
    bool decode_h264_frame(const uint8_t* data, size_t size, 
                          GLuint& texture_out);
    
    // Texture handling
    void upload_texture(const uint8_t* rgba_data, int width, int height);
    
    // FPS tracking
    void update_fps();
    
    // Current state
    int frame_width_ = 0;
    int frame_height_ = 0;
    float last_fps_ = 0.0f;
    std::chrono::steady_clock::time_point last_fps_update_;
    
    // OpenGL resources
    GLuint h264_decoder_texture_ = 0;
    GLuint framebuffer_object_ = 0;
};
