#include "display_renderer.hpp"
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <cmath>

DisplayRenderer::DisplayRenderer(QWidget* parent) 
    : QOpenGLWidget(QSurfaceFormat::defaultFormat(), parent),
      last_fps_update_(std::chrono::steady_clock::now()) {}

DisplayRenderer::~DisplayRenderer() {
    makeCurrent();
    if (h264_decoder_texture_ != 0) {
        glDeleteTextures(1, &h264_decoder_texture_);
    }
    if (framebuffer_object_ != 0) {
        glDeleteFramebuffers(1, &framebuffer_object_);
    }
    doneCurrent();
}

void DisplayRenderer::initializeGL() {
    initializeOpenGLFunctions();
    
    // Enable features
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Set up viewport
    QRect rect = geometry();
    glViewport(0, 0, rect.width(), rect.height());
    
    // Create texture for frame rendering
    glGenTextures(1, &h264_decoder_texture_);
    glBindTexture(GL_TEXTURE_2D, h264_decoder_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup framebuffer object for offscreen rendering (optional)
    glGenFramebuffers(1, &framebuffer_object_);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_object_);
    
    std::cout << "DisplayRenderer initialized" << std::endl;
}

void DisplayRenderer::paintGL() {
    // Clear buffer
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // If we have a valid texture from the decoder, render it
    if (h264_decoder_texture_ != 0) {
        // This is a simplified renderer - in production you'd use proper YUV->RGB conversion
        // For MVP, we'll just show a placeholder color
        glColor4f(0.2f, 0.3f, 0.4f, 1.0f);
        
        // Draw quad
        glBegin(GL_QUADS);
        glVertex2f(-1.0f, -1.0f);
        glVertex2f(1.0f, -1.0f);
        glVertex2f(1.0f, 1.0f);
        glVertex2f(-1.0f, 1.0f);
        glEnd();
    }
    
    update_fps();
}

void DisplayRenderer::resizeGL(int width, int height) {
    makeCurrent();
    
    QRect rect = geometry();
    glViewport(0, 0, rect.width(), rect.height());
    
    doneCurrent();
}

void DisplayRenderer::render_frame(const uint8_t* h264_data, size_t data_size) {
    if (!h264_data || data_size == 0) {
        return;
    }
    
    // For MVP, we'll simulate frame reception without actual decoding
    // In production, this would call: decode_h264_frame()
    
    if (frame_width_ == 0 || frame_height_ == 0) {
        // Use default resolution
        frame_width_ = 1920;
        frame_height_ = 1080;
    }
    
    // Update FPS tracking
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_fps_update_).count();
    
    if (duration >= 500) {  // Update every 500ms
        float fps = 30.0f;  // Assume constant 30fps for demo
        emit fps_changed(fps);
        
        last_fps_update_ = now;
        last_fps_ = fps;
    }
}

bool DisplayRenderer::decode_h264_frame(const uint8_t* data, size_t size, GLuint& tex_out) {
    // For MVP, simplified version without actual FFmpeg integration
    // Real implementation would require libavcodec
    
    // This would parse NAL units, decode, convert YUV->RGB, and upload to texture
    // For now, return false to indicate no actual decoding happened
    return false;
}

void DisplayRenderer::upload_texture(const uint8_t* rgba_data, int width, int height) {
    makeCurrent();
    
    glBindTexture(GL_TEXTURE_2D, h264_decoder_texture_);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 
                 width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba_data);
    
    doneCurrent();
}

void DisplayRenderer::handle_mouse_event(int x, int y, bool button_pressed, 
                                         unsigned int button_flags) {
    // Forward mouse events to remote control session
    // This would typically emit a signal back to the main window
    // which would then send the event through the tunnel manager
}

void DisplayRenderer::set_frame_size(int width, int height) {
    makeCurrent();
    
    frame_width_ = width;
    frame_height_ = height;
    
    QRect rect = geometry();
    glViewport(0, 0, rect.width(), rect.height());
    
    doneCurrent();
}

void DisplayRenderer::update_fps() {
    // Already tracked in render_frame, but keep for potential future enhancement
}
