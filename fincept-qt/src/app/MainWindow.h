#pragma once
#include "ai_chat/AiChatBubble.h"
#include "app/ScreenRouter.h"

#include <QMainWindow>
#include <QStackedWidget>
#include <QTimer>

namespace ads {
class CDockManager;
}
namespace fincept {
class DockScreenRouter;
}
namespace fincept::ui {
class DockToolBar;
class DockStatusBar;
class TabBar;
} // namespace fincept::ui

namespace fincept {

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(int window_id = 0, QWidget* parent = nullptr);

    int window_id() const { return window_id_; }

    static int next_window_id() {
        static int s_id = 1;
        return s_id++;
    }

  protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    QStackedWidget* stack_ = nullptr;
    int window_id_ = 0;

    ads::CDockManager* dock_manager_ = nullptr;
    DockScreenRouter* dock_router_ = nullptr;
    ui::DockToolBar* dock_toolbar_ = nullptr;
    ui::DockStatusBar* dock_status_bar_ = nullptr;
    ui::TabBar* tab_bar_ = nullptr;

    bool focus_mode_ = false;
    bool chat_mode_ = false;
    bool always_on_top_ = false;
    AiChatBubble* chat_bubble_ = nullptr;

    QTimer* dock_layout_save_timer_ = nullptr;
    void schedule_dock_layout_save();

    void setup_auth_screens();
    void setup_app_screens();
    void setup_docking_mode();
    void setup_dock_screens();
    void setup_navigation();
    void on_auth_state_changed();
    void toggle_chat_mode();
    void show_lock_screen();
    void on_terminal_unlocked();
    void update_window_title();
    void set_shell_visible(bool visible);

  private slots:
    void show_login();
    void show_register();
    void show_forgot_password();
    void show_pricing();
    void show_info_contact();
    void show_info_terms();
    void show_info_privacy();
    void show_info_trademarks();
    void show_info_help();
};

} // namespace fincept
