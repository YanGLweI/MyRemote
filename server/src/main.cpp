// Server Control - Main Entry Point with Qt GUI
#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>
#include <thread>
#include "listener.hpp"
#include "tunnel_manager.hpp"
#include "device_list.hpp"
#include "display_renderer.hpp"
#include "remote_controller.hpp"
#include "../common/include/config.hpp"

class MainWindow : public QMainWindow {
    Q_OBJECT
    
public:
    MainWindow() {
        // Load configuration from file if available
        try {
            auto config = config::load_server_config("server_config.json");
            server_port_ = config.listening_port;
            max_clients_ = config.max_connections;
            
            std::cout << "Server configuration loaded:" << std::endl;
            std::cout << "  Listening port: " << server_port_ << std::endl;
            std::cout << "  Max clients: " << max_clients_ << std::endl;
        } catch (...) {
            std::cout << "Using default server configuration (port 7500)" << std::endl;
        }
        
        // Initialize tunnel manager
        tunnel_manager_ = std::make_unique<TunnelManager>();
        if (!tunnel_manager_->start(server_port_)) {
            QMessageBox::critical(this, "Error", 
                "Failed to start server listener on port " + QString::number(server_port_));
            QApplication::quit();
            return;
        }
        
        // Initialize remote controller
        remote_controller_ = std::make_unique<RemoteController>(*tunnel_manager_);
        
        // Connect device list signals
        connect(remote_controller_.get(), &RemoteController::control_started, 
                this, [this](const std::string& device_id) {
            on_start_remote_control(device_id);
        });
        
        // Create UI layout
        auto* central_widget = new QWidget();
        setCentralWidget(central_widget);
        
        auto* main_layout = new QVBoxLayout(central_widget);
        
        // Device list panel
        device_list_ = new DeviceListWidget();
        device_list_->setMinimumWidth(250);
        device_list_->setMaximumWidth(300);
        main_layout->addWidget(device_list_);
        
        // Remote display panel (empty initially)
        status_label_ = new QLabel("Select a device to start remote control");
        status_label_->setAlignment(Qt::AlignCenter);
        status_label_->setStyleSheet("color: gray; font-style: italic; background-color: #f0f0f0;");
        status_label_->setMinimumHeight(30);
        main_layout->addWidget(status_label_);
        
        // Display renderer widget
        display_renderer_ = new DisplayRenderer();
        display_renderer_->setMinimumSize(640, 480);
        display_renderer_->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        display_renderer_->setVisible(false);  // Hidden until remote starts
        
        // Add FPS indicator
        fps_label_ = new QLabel("FPS: --");
        fps_label_->setFixedSize(80, 20);
        fps_label_->setAlignment(Qt::AlignCenter | Qt::AlignBottom);
        fps_label_->setStyleSheet("font-weight: bold; color: #2E8B57;");
        
        auto* display_wrapper = new QWidget();
        auto* display_layout = new QVBoxLayout(display_wrapper);
        display_layout->addWidget(fps_label_, 0, Qt::AlignRight);
        display_layout->addWidget(display_renderer_);
        display_layout->setContentsMargins(0, 0, 0, 0);
        
        main_layout->addWidget(display_wrapper);
        
        // Buttons
        auto* button_layout = new QHBoxLayout();
        
        refresh_button_ = new QPushButton("Refresh Devices");
        connect(refresh_button_, &QPushButton::clicked, this, &MainWindow::refresh_devices);
        button_layout->addWidget(refresh_button_);
        
        stop_button_ = new QPushButton("Stop Control");
        stop_button_->setEnabled(false);
        connect(stop_button_, &QPushButton::clicked, this, [this]() {
            stop_remote_control();
        });
        button_layout->addWidget(stop_button_);
        
        quit_button_ = new QPushButton("Quit");
        connect(quit_button_, &QPushButton::clicked, []() {
            QApplication::quit();
        });
        button_layout->addWidget(quit_button_);
        
        main_layout->addLayout(button_layout);
        
        // Status bar
        statusBar()->showMessage("Ready", 0);
        
        setWindowTitle("MyRemote Control Center v1.0");
        resize(900, 700);
        
        // Start device refresh timer (poll every second)
        QTimer* refresh_timer = new QTimer(this);
        connect(refresh_timer, &QTimer::timeout, this, &MainWindow::refresh_devices);
        refresh_timer->start(1000);  // Refresh every second
        
        // Populate initial test devices
        populate_test_devices();
        
        std::cout << "Control center ready, accepting connections..." << std::endl;
    }
    
    ~MainWindow() {}
    
private slots:
    void refresh_devices() {
        auto devices = tunnel_manager_->get_all_clients();
        device_list_->clear_devices();
        
        for (const auto& device : devices) {
            device_list_->add_device(
                device->device_id,
                device->device_name,
                device->screen_width,
                device->screen_height,
                device->active,
                device->connect_time
            );
        }
        
        status_label_->setText(QString("%1 device(s) online")
            .arg(static_cast<int>(devices.size())));
    }
    
    void on_start_remote_control(const std::string& device_id) {
        std::cout << "Starting remote control for device: " << device_id << std::endl;
        
        // Check if already controlling
        if (remote_controller_->is_controlling()) {
            QMessageBox::warning(this, "Already Controlling",
                "Already remotely controlling another device. Stop first.");
            return;
        }
        
        // Start remote control session
        bool success = remote_controller_->start_remote_control(device_id);
        
        if (!success) {
            QMessageBox::critical(this, "Connection Error",
                QString("Failed to connect to device: %1").arg(QString::fromStdString(device_id)));
            return;
        }
        
        // Show display renderer
        display_renderer_->setVisible(true);
        stop_button_->setEnabled(true);
        
        status_label_->setText(QString("Connected to: %1 - Send mouse/keyboard events to control").
            arg(QString::fromStdString(device_id)));
        status_label_->setStyleSheet("color: #2E8B57; font-weight: bold; background-color: #e0ffe0;");
        
        statusBar()->showMessage(QString("Controlling %1").arg(QString::fromStdString(device_id)), 0);
        
        std::cout << "Remote control session established" << std::endl;
        
        // TODO: Start receiving frame stream from device_id
        // This would involve listening for frames and forwarding to display_renderer
    }
    
    void stop_remote_control() {
        std::cout << "Stopping remote control session" << std::endl;
        
        remote_controller_->stop_remote_control();
        
        display_renderer_->setVisible(false);
        stop_button_->setEnabled(false);
        
        status_label_->setText("Remote control stopped");
        status_label_->setStyleSheet("color: gray; font-style: italic; background-color: #f0f0f0;");
        
        fps_label_->setText("FPS: --");
        
        statusBar()->showMessage("Remote control stopped", 5000);
        
        std::cout << "Remote control session terminated" << std::endl;
    }
    
private:
    std::unique_ptr<TunnelManager> tunnel_manager_;
    std::unique_ptr<RemoteController> remote_controller_;
    DeviceListWidget* device_list_;
    DisplayRenderer* display_renderer_;
    QLabel* status_label_;
    QLabel* fps_label_;
    QPushButton* refresh_button_;
    QPushButton* stop_button_;
    QPushButton* quit_button_;
    
    int server_port_ = 7500;
    int max_clients_ = 50;
    
    void populate_test_devices() {
        // Test devices for development/demo purposes
        time_t now = std::time(nullptr);
        
        device_list_->add_device("device_001", "Workstation-Alpha", 
                                 1920, 1080, true, now);
        device_list_->add_device("device_002", "Office-Beta", 
                                 1366, 768, true, now - 300);
        device_list_->add_device("device_003", "Lab-Gamma", 
                                 2560, 1440, false, now - 3600);
        
        std::cout << "Populated " << device_list_->findChildren<QListWidgetItem*>().size() 
                  << " test devices" << std::endl;
    }
    
signals:
    void fps_updated(float fps);
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("MyRemote Control Center");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("MyRemote");
    
    MainWindow window;
    window.show();
    
    return app.exec();
}

#include "main.moc"
