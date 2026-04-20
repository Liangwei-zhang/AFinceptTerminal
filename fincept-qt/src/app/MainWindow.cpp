#include "app/MainWindow.h"

#include "ai_chat/AiChatBubble.h"
#include "ai_chat/AiChatScreen.h"
#include "app/DockScreenRouter.h"
#include "core/config/ProfileManager.h"
#include "core/events/EventBus.h"
#include "core/keys/KeyConfigManager.h"
#include "core/logging/Logger.h"
#include "core/session/SessionManager.h"
#include "screens/about/AboutScreen.h"
#include "screens/algo_trading/AlgoTradingScreen.h"
#include "screens/backtesting/BacktestingScreen.h"
#include "screens/dashboard/DashboardScreen.h"
#include "screens/docs/DocsScreen.h"
#include "screens/equity_research/EquityResearchScreen.h"
#include "screens/equity_trading/EquityTradingScreen.h"
#include "screens/file_manager/FileManagerScreen.h"
#include "screens/info/ContactScreen.h"
#include "screens/info/HelpScreen.h"
#include "screens/info/PrivacyScreen.h"
#include "screens/info/TermsScreen.h"
#include "screens/info/TrademarksScreen.h"
#include "screens/markets/MarketsScreen.h"
#include "screens/news/NewsScreen.h"
#include "screens/notes/NotesScreen.h"
#include "screens/portfolio/PortfolioScreen.h"
#include "screens/profile/ProfileScreen.h"
#include "screens/settings/SettingsScreen.h"
#include "screens/watchlist/WatchlistScreen.h"
#include "services/updater/UpdateService.h"
#include "services/workspace/WorkspaceManager.h"
#include "storage/repositories/SettingsRepository.h"
#include "trading/instruments/InstrumentService.h"
#include "ui/navigation/DockStatusBar.h"
#include "ui/navigation/DockToolBar.h"
#include "ui/theme/Theme.h"
#include "ui/workspace/WorkspaceNewDialog.h"
#include "ui/workspace/WorkspaceOpenDialog.h"
#include "ui/workspace/WorkspaceSaveAsDialog.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QPalette>
#include <QScreen>
#include <QVBoxLayout>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <FloatingDockContainer.h>

namespace fincept {

MainWindow::MainWindow(int window_id, QWidget* parent) : QMainWindow(parent), window_id_(window_id) {
    const QString profile = ProfileManager::instance().active();
    setWindowTitle(profile == "default" ? "Fincept Terminal" : QString("Fincept Terminal [%1]").arg(profile));

    QIcon app_icon;
#if defined(Q_OS_WIN)
    app_icon = QApplication::windowIcon();
    if (app_icon.isNull()) {
        const QString ico = QCoreApplication::applicationDirPath() + "/fincept.ico";
        if (QFile::exists(ico))
            app_icon = QIcon(ico);
    }
#else
    {
        const QString ico = QCoreApplication::applicationDirPath() + "/fincept.ico";
        if (QFile::exists(ico))
            app_icon = QIcon(ico);
    }
#endif
    if (!app_icon.isNull())
        setWindowIcon(app_icon);

    setMinimumSize(800, 500);

    const QByteArray saved_geom = SessionManager::instance().load_geometry(window_id_);
    if (!saved_geom.isEmpty()) {
        restoreGeometry(saved_geom);
    } else {
        QScreen* screen = QApplication::primaryScreen();
        if (screen) {
            const QRect geom = screen->availableGeometry();
            resize(geom.width() * 9 / 10, geom.height() * 9 / 10);
            if (window_id_ == 0)
                move(geom.center() - rect().center());
        }
    }

    auto* master_stack = new QStackedWidget;
    setup_docking_mode();
    master_stack->addWidget(dock_manager_->parentWidget()); // index 0 — main workspace only
    setCentralWidget(master_stack);
    stack_ = master_stack;

    dock_toolbar_ = new ui::DockToolBar(this);
    addToolBar(Qt::TopToolBarArea, dock_toolbar_);

    dock_status_bar_ = new ui::DockStatusBar(this);
    setStatusBar(dock_status_bar_);
    auto* toolbar = dock_toolbar_->inner();

    WorkspaceManager::instance().set_main_window(this);
    connect(&WorkspaceManager::instance(), &WorkspaceManager::workspace_loaded, this,
            [this](const WorkspaceDef&) { update_window_title(); });
    connect(&WorkspaceManager::instance(), &WorkspaceManager::workspace_error, this,
            [this](const QString& msg) { QMessageBox::warning(this, "Workspace Error", msg); });

    dock_router_ = new DockScreenRouter(dock_manager_, this);
    setup_dock_screens();

    connect(tab_bar_, &ui::TabBar::tab_changed, this, [this](const QString& id) { dock_router_->navigate(id, true); });
    connect(dock_router_, &DockScreenRouter::screen_changed, tab_bar_, &ui::TabBar::set_active);
    connect(dock_router_, &DockScreenRouter::screen_changed, this, [this](const QString&) { update_window_title(); });
    connect(this, &QObject::destroyed, this, [this]() { WorkspaceManager::instance().remove_window(this); });

    chat_bubble_ = new AiChatBubble(dock_manager_);
    {
        const auto r = SettingsRepository::instance().get("appearance.show_chat_bubble");
        const bool show = !r.is_ok() || r.value() != "false";
        chat_bubble_->setVisible(show);
        if (show)
            chat_bubble_->raise();
    }
    connect(dock_router_, &DockScreenRouter::screen_changed, this, [this](const QString&) {
        if (!chat_bubble_)
            return;
        const auto r = SettingsRepository::instance().get("appearance.show_chat_bubble");
        const bool show = !r.is_ok() || r.value() != "false";
        chat_bubble_->setVisible(show);
        if (show) {
            chat_bubble_->reposition();
            chat_bubble_->raise();
        }
    });

    connect(toolbar, &ui::ToolBar::chat_mode_toggled, this, &MainWindow::toggle_chat_mode);
    connect(toolbar, &ui::ToolBar::navigate_to, this,
            [this](const QString& id) { dock_router_->navigate(id, true); });

    dock_layout_save_timer_ = new QTimer(this);
    dock_layout_save_timer_->setSingleShot(true);
    dock_layout_save_timer_->setInterval(500);
    connect(dock_layout_save_timer_, &QTimer::timeout, this, [this]() {
        if (!dock_manager_)
            return;
        SessionManager::instance().save_dock_layout(window_id_, dock_manager_->saveState());
        if (WorkspaceManager::instance().has_current_workspace())
            WorkspaceManager::instance().save_workspace();
    });

    connect(toolbar, &ui::ToolBar::dock_command, this,
            [this](const QString& action, const QString& primary, const QString& secondary) {
                if (action == "add")
                    dock_router_->add_alongside(primary, secondary);
                else if (action == "remove")
                    dock_router_->remove_screen(primary);
                else if (action == "replace")
                    dock_router_->replace_screen(primary, secondary);
                schedule_dock_layout_save();
            });

    EventBus::instance().subscribe("nav.switch_screen", [this](const QVariantMap& data) {
        const QString screen_id = data["screen_id"].toString();
        if (!screen_id.isEmpty()) {
            QMetaObject::invokeMethod(dock_router_, [this, screen_id]() { dock_router_->navigate(screen_id); },
                                      Qt::QueuedConnection);
        }
    });

    auto& km = KeyConfigManager::instance();

    auto* act_chat = km.action(KeyAction::ToggleChat);
    act_chat->setShortcutContext(Qt::WindowShortcut);
    addAction(act_chat);
    connect(act_chat, &QAction::triggered, this, &MainWindow::toggle_chat_mode);

    auto* act_fs = km.action(KeyAction::Fullscreen);
    act_fs->setShortcutContext(Qt::WindowShortcut);
    addAction(act_fs);
    connect(act_fs, &QAction::triggered, this, [this]() {
        if (isFullScreen())
            showNormal();
        else
            showFullScreen();
    });

    auto* act_focus = km.action(KeyAction::FocusMode);
    act_focus->setShortcutContext(Qt::WindowShortcut);
    addAction(act_focus);
    connect(act_focus, &QAction::triggered, this, [this]() {
        focus_mode_ = !focus_mode_;
        if (dock_toolbar_)
            dock_toolbar_->setVisible(!focus_mode_);
        if (dock_status_bar_)
            dock_status_bar_->setVisible(!focus_mode_);
    });

    auto* act_refresh = km.action(KeyAction::Refresh);
    act_refresh->setShortcutContext(Qt::WindowShortcut);
    addAction(act_refresh);
    connect(act_refresh, &QAction::triggered, this, [this]() {
        if (!dock_manager_)
            return;
        auto* focused = dock_manager_->focusedDockWidget();
        if (focused && focused->widget())
            QMetaObject::invokeMethod(focused->widget(), "refresh", Qt::QueuedConnection);
    });

    auto* act_screenshot = km.action(KeyAction::Screenshot);
    act_screenshot->setShortcutContext(Qt::WindowShortcut);
    addAction(act_screenshot);
    connect(act_screenshot, &QAction::triggered, this, [this]() {
        QScreen* scr = this->screen();
        if (!scr)
            scr = QApplication::primaryScreen();
        const QPixmap px = scr->grabWindow(winId());
        const QString path = QFileDialog::getSaveFileName(
            this, "Save Screenshot",
            QDir::homePath() + "/fincept_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png",
            "PNG Images (*.png)");
        if (!path.isEmpty()) {
            px.save(path, "PNG");
            LOG_INFO("MainWindow", QString("Screenshot saved: %1").arg(path));
        }
    });

    connect(toolbar, &ui::ToolBar::action_triggered, this, [this](const QString& action) {
        if (action == "new_window") {
            auto* w = new MainWindow(MainWindow::next_window_id());
            w->setAttribute(Qt::WA_DeleteOnClose);
            const QList<QScreen*> screens = QGuiApplication::screens();
            if (screens.size() > 1) {
                const QPoint this_centre = geometry().center();
                for (QScreen* s : screens) {
                    if (!s->geometry().contains(this_centre)) {
                        const QRect sg = s->availableGeometry();
                        w->resize(sg.width() * 9 / 10, sg.height() * 9 / 10);
                        w->move(sg.center() - w->rect().center());
                        break;
                    }
                }
            }
            w->show();
            w->raise();
            w->activateWindow();
        } else if (action.startsWith("panel_")) {
            static const QMap<QString, QString> panel_map = {
                {"panel_dashboard", "dashboard"},
                {"panel_watchlist", "watchlist"},
                {"panel_news", "news"},
                {"panel_portfolio", "portfolio"},
                {"panel_markets", "markets"},
                {"panel_equity", "equity_trading"},
                {"panel_algo", "algo_trading"},
                {"panel_research", "equity_research"},
                {"panel_ai_chat", "ai_chat"},
            };
            if (panel_map.contains(action) && dock_router_)
                dock_router_->navigate(panel_map[action]);
        } else if (action == "perspective_save") {
            if (dock_manager_) {
                bool ok = false;
                const QString name =
                    QInputDialog::getText(this, "Save Layout", "Layout name:", QLineEdit::Normal, QString(), &ok);
                if (ok && !name.trimmed().isEmpty()) {
                    dock_manager_->addPerspective(name.trimmed());
                    LOG_INFO("MainWindow", QString("Saved perspective: %1").arg(name.trimmed()));
                }
            }
        } else if (action.startsWith("perspective_")) {
            static const QMap<QString, QStringList> view_screens = {
                {"perspective_markets", {"markets", "watchlist"}},
                {"perspective_research", {"equity_research", "markets", "news"}},
                {"perspective_equity", {"equity_trading", "watchlist", "news"}},
                {"perspective_portfolio", {"portfolio", "markets", "watchlist", "news"}},
                {"perspective_algo", {"algo_trading", "backtesting"}},
                {"perspective_ai", {"ai_chat", "notes"}},
            };
            const auto it = view_screens.find(action);
            if (it != view_screens.end() && dock_router_) {
                const QStringList& screens = it.value();
                if (!screens.isEmpty()) {
                    dock_router_->navigate(screens[0], true);
                    for (int i = 1; i < screens.size(); ++i)
                        dock_router_->navigate(screens[i]);
                }
                LOG_INFO("MainWindow", QString("Quick Switch: %1 (%2 panels)").arg(action).arg(screens.size()));
            }
        } else if (action == "fullscreen") {
            if (isFullScreen())
                showNormal();
            else
                showFullScreen();
        } else if (action == "focus_mode") {
            focus_mode_ = !focus_mode_;
            if (dock_toolbar_)
                dock_toolbar_->setVisible(!focus_mode_);
            if (dock_status_bar_)
                dock_status_bar_->setVisible(!focus_mode_);
        } else if (action == "always_on_top") {
            always_on_top_ = !always_on_top_;
            Qt::WindowFlags flags = windowFlags();
            if (always_on_top_)
                flags |= Qt::WindowStaysOnTopHint;
            else
                flags &= ~Qt::WindowStaysOnTopHint;
            setWindowFlags(flags);
            show();
        } else if (action == "refresh") {
            if (!dock_manager_)
                return;
            auto* focused = dock_manager_->focusedDockWidget();
            if (focused && focused->widget())
                QMetaObject::invokeMethod(focused->widget(), "refresh", Qt::QueuedConnection);
        } else if (action == "new_workspace") {
            auto* dlg = new ui::WorkspaceNewDialog(this);
            if (dlg->exec() == QDialog::Accepted)
                WorkspaceManager::instance().new_workspace(dlg->workspace_name(), dlg->workspace_description(),
                                                           dlg->selected_template_id());
            dlg->deleteLater();
        } else if (action == "open_workspace") {
            auto* dlg = new ui::WorkspaceOpenDialog(this);
            if (dlg->exec() == QDialog::Accepted && !dlg->selected_path().isEmpty())
                WorkspaceManager::instance().open_workspace(dlg->selected_path());
            dlg->deleteLater();
        } else if (action == "save_workspace") {
            WorkspaceManager::instance().save_workspace();
        } else if (action == "save_workspace_as") {
            auto* dlg = new ui::WorkspaceSaveAsDialog(this);
            if (dlg->exec() == QDialog::Accepted)
                WorkspaceManager::instance().save_workspace_as(dlg->new_name(), dlg->chosen_path());
            dlg->deleteLater();
        } else if (action == "import_data") {
            const QString path =
                QFileDialog::getOpenFileName(this, "Import Workspace", QDir::homePath(), "Fincept Workspace (*.fwsp)");
            if (!path.isEmpty())
                WorkspaceManager::instance().import_workspace(path);
        } else if (action == "export_data") {
            const QString path =
                QFileDialog::getSaveFileName(this, "Export Workspace", QDir::homePath(), "Fincept Workspace (*.fwsp)");
            if (!path.isEmpty())
                WorkspaceManager::instance().export_workspace(path);
        } else if (action == "screenshot") {
            QScreen* scr = this->screen();
            if (!scr)
                scr = QApplication::primaryScreen();
            const QPixmap px = scr->grabWindow(winId());
            const QString path = QFileDialog::getSaveFileName(
                this, "Save Screenshot",
                QDir::homePath() + "/fincept_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png",
                "PNG Images (*.png)");
            if (!path.isEmpty()) {
                px.save(path, "PNG");
                LOG_INFO("MainWindow", QString("Screenshot saved: %1").arg(path));
            }
        } else if (action == "check_updates") {
            services::UpdateService::instance().set_dialog_parent(this);
            services::UpdateService::instance().check_for_updates(false);
        }
    });

    const QByteArray saved_state = SessionManager::instance().load_state(window_id_);
    if (!saved_state.isEmpty())
        restoreState(saved_state);

    if (dock_toolbar_)
        dock_toolbar_->setVisible(true);
    if (dock_status_bar_)
        dock_status_bar_->setVisible(true);

    static constexpr int kDockLayoutVersion = 5;
    bool dock_restored = false;

    if (dock_manager_) {
        QSettings persp_settings;
        SessionManager::instance().load_perspectives(persp_settings);
        dock_manager_->loadPerspectives(persp_settings);

        const int saved_version = SessionManager::instance().dock_layout_version(window_id_);
        if (saved_version == kDockLayoutVersion) {
            const QByteArray saved_dock = SessionManager::instance().load_dock_layout(window_id_);
            if (!saved_dock.isEmpty()) {
                dock_router_->ensure_all_registered();
                dock_restored = dock_manager_->restoreState(saved_dock);
                if (dock_restored && dock_manager_->openedDockAreas().size() > 6) {
                    LOG_WARN("MainWindow", QString("Dock layout corrupt: %1 open areas — resetting")
                                               .arg(dock_manager_->openedDockAreas().size()));
                    dock_restored = false;
                }
            }
        } else if (saved_version != 0) {
            LOG_INFO("MainWindow", QString("Dock layout version mismatch (saved %1, expected %2) — resetting")
                                       .arg(saved_version)
                                       .arg(kDockLayoutVersion));
        }

        if (!dock_restored) {
            for (auto* dw : dock_manager_->dockWidgetsMap())
                dw->toggleView(false);
        }

        SessionManager::instance().set_dock_layout_version(window_id_, kDockLayoutVersion);
    }

    set_shell_visible(true);
    stack_->setCurrentIndex(0);
    WorkspaceManager::instance().load_last_workspace();

    if (!dock_restored) {
        dock_router_->navigate("dashboard");
        LOG_INFO("MainWindow", "Applied clean default dock layout");
    } else {
        const QString last = SessionManager::instance().last_screen();
        if (!last.isEmpty()) {
            if (auto* dw = dock_router_->find_dock_widget(last); dw && !dw->isClosed()) {
                dw->raise();
                dw->setAsCurrentTab();
            }
            tab_bar_->set_active(last);
        }
    }

    QTimer::singleShot(3000, this, [this]() {
        services::UpdateService::instance().set_dialog_parent(this);
        services::UpdateService::instance().check_for_updates(true);
    });
    fincept::trading::InstrumentService::instance().load_from_db_async("zerodha");
    fincept::trading::InstrumentService::instance().load_from_db_async("angelone");
    fincept::trading::InstrumentService::instance().load_from_db_async("groww");
}

void MainWindow::toggle_chat_mode() {
    if (dock_router_)
        dock_router_->navigate("ai_chat", true);
}

void MainWindow::setup_auth_screens() {}

void MainWindow::setup_docking_mode() {
    static bool s_ads_configured = false;
    if (!s_ads_configured) {
        ads::CDockManager::setConfigFlags(
            ads::CDockManager::DefaultOpaqueConfig |
            ads::CDockManager::AlwaysShowTabs | ads::CDockManager::DockAreaHasTabsMenuButton |
            ads::CDockManager::DockAreaDynamicTabsMenuButtonVisibility |
            ads::CDockManager::FloatingContainerHasWidgetTitle | ads::CDockManager::FloatingContainerHasWidgetIcon |
            ads::CDockManager::EqualSplitOnInsertion | ads::CDockManager::DoubleClickUndocksWidget);
        ads::CDockManager::setAutoHideConfigFlags(ads::CDockManager::AutoHideFlags(0));
        s_ads_configured = true;
    }

    auto* dock_wrapper = new QWidget;
    {
        QPalette pal = dock_wrapper->palette();
        pal.setColor(QPalette::Window, QColor(ui::colors::BG_BASE()));
        dock_wrapper->setPalette(pal);
        dock_wrapper->setAutoFillBackground(true);
    }
    auto* dock_layout = new QVBoxLayout(dock_wrapper);
    dock_layout->setContentsMargins(0, 0, 0, 0);
    dock_layout->setSpacing(0);

    tab_bar_ = new ui::TabBar(dock_wrapper);
    dock_layout->addWidget(tab_bar_);

    dock_manager_ = new ads::CDockManager(dock_wrapper);
    dock_manager_->setStyleSheet(ui::ThemeManager::instance().build_ads_qss());
    dock_layout->addWidget(dock_manager_);

    connect(dock_manager_, &ads::CDockManager::dockAreaCreated, this,
            [this](ads::CDockAreaWidget*) { schedule_dock_layout_save(); });
    connect(dock_manager_, &ads::CDockManager::dockWidgetAdded, this,
            [this](ads::CDockWidget*) { schedule_dock_layout_save(); });
    connect(dock_manager_, &ads::CDockManager::dockWidgetRemoved, this,
            [this](ads::CDockWidget*) { schedule_dock_layout_save(); });
    connect(dock_manager_, &ads::CDockManager::floatingWidgetCreated, this,
            [this](ads::CFloatingDockContainer*) { schedule_dock_layout_save(); });
}

void MainWindow::setup_dock_screens() {
    dock_router_->register_factory("dashboard", []() { return new screens::DashboardScreen; });
    dock_router_->register_factory("markets", []() { return new screens::MarketsScreen; });
    dock_router_->register_factory("news", []() { return new screens::NewsScreen; });
    dock_router_->register_factory("watchlist", []() { return new screens::WatchlistScreen; });
    dock_router_->register_factory("equity_research", []() { return new screens::EquityResearchScreen; });

    dock_router_->register_factory("portfolio", []() { return new screens::PortfolioScreen; });
    dock_router_->register_factory("equity_trading", []() { return new screens::EquityTradingScreen; });
    dock_router_->register_factory("algo_trading", []() { return new screens::AlgoTradingScreen; });
    dock_router_->register_factory("backtesting", []() { return new screens::BacktestingScreen; });

    dock_router_->register_factory("ai_chat", []() { return new screens::AiChatScreen; });
    dock_router_->register_factory("notes", []() { return new screens::NotesScreen; });
    dock_router_->register_factory("profile", []() { return new screens::ProfileScreen; });
    dock_router_->register_factory("settings", []() { return new screens::SettingsScreen; });
    dock_router_->register_factory("about", []() { return new screens::AboutScreen; });
    dock_router_->register_factory("docs", []() { return new screens::DocsScreen; });

    dock_router_->register_factory("file_manager", [this]() {
        auto* fm = new screens::FileManagerScreen;
        connect(fm, &screens::FileManagerScreen::open_file_in_screen, this,
                [this](const QString& route_id, const QString&) { dock_router_->navigate(route_id); });
        return fm;
    });

    dock_router_->register_screen("contact", new screens::ContactScreen);
    dock_router_->register_screen("terms", new screens::TermsScreen);
    dock_router_->register_screen("privacy", new screens::PrivacyScreen);
    dock_router_->register_screen("trademarks", new screens::TrademarksScreen);
    dock_router_->register_screen("help", new screens::HelpScreen);
}

void MainWindow::setup_app_screens() {}
void MainWindow::setup_navigation() {}
void MainWindow::on_auth_state_changed() {}
void MainWindow::show_login() {}
void MainWindow::show_register() {}
void MainWindow::show_forgot_password() {}
void MainWindow::show_pricing() {}
void MainWindow::show_info_contact() { if (dock_router_) dock_router_->navigate("contact", true); }
void MainWindow::show_info_terms() { if (dock_router_) dock_router_->navigate("terms", true); }
void MainWindow::show_info_privacy() { if (dock_router_) dock_router_->navigate("privacy", true); }
void MainWindow::show_info_trademarks() { if (dock_router_) dock_router_->navigate("trademarks", true); }
void MainWindow::show_info_help() { if (dock_router_) dock_router_->navigate("help", true); }
void MainWindow::show_lock_screen() {}
void MainWindow::on_terminal_unlocked() {}

void MainWindow::set_shell_visible(bool visible) {
    if (dock_toolbar_)
        dock_toolbar_->setVisible(visible && !focus_mode_ && !chat_mode_);
    if (dock_status_bar_)
        dock_status_bar_->setVisible(visible && !focus_mode_ && !chat_mode_);
    if (!visible) {
        const QString profile = ProfileManager::instance().active();
        setWindowTitle(profile == "default" ? "Fincept Terminal" : QString("Fincept Terminal [%1]").arg(profile));
    }
}

void MainWindow::update_window_title() {
    QString title = "Fincept Terminal";

    const QString profile = ProfileManager::instance().active();
    if (profile != "default")
        title += QString(" [%1]").arg(profile);

    if (WorkspaceManager::instance().has_current_workspace()) {
        const QString ws = WorkspaceManager::instance().current_workspace_name();
        if (!ws.isEmpty())
            title += QString(" — %1").arg(ws);
    }

    if (dock_router_) {
        const QString id = dock_router_->current_screen_id();
        if (!id.isEmpty())
            title += QString(" — %1").arg(DockScreenRouter::title_for_id(id));
    }

    setWindowTitle(title);
}

void MainWindow::schedule_dock_layout_save() {
    if (dock_layout_save_timer_)
        dock_layout_save_timer_->start();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    WorkspaceManager::instance().save_workspace();
    SessionManager::instance().save_geometry(window_id_, saveGeometry(), saveState());
    if (dock_manager_) {
        SessionManager::instance().save_dock_layout(window_id_, dock_manager_->saveState());
        QSettings tmp;
        dock_manager_->savePerspectives(tmp);
        SessionManager::instance().save_perspectives(tmp);
    }
    event->accept();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (chat_bubble_)
        chat_bubble_->reposition();
}

} // namespace fincept
