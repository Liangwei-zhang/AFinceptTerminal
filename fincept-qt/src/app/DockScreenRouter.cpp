#include "app/DockScreenRouter.h"

#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "core/session/SessionManager.h"
#include "screens/IStatefulScreen.h"

#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <DockWidgetTab.h>

namespace fincept {

QString DockScreenRouter::title_for_id(const QString& id) {
    static const QHash<QString, QString> titles = {
        {"dashboard", "Dashboard"},
        {"markets", "Markets"},
        {"crypto_trading", "Crypto Trading"},
        {"equity_trading", "Equity Trading"},
        {"algo_trading", "Algo Trading"},
        {"backtesting", "Backtesting"},
        {"portfolio", "Portfolio"},
        {"watchlist", "Watchlist"},
        {"news", "News"},
        {"ai_chat", "AI Chat"},
        {"equity_research", "Equity Research"},
        {"economics", "Economics"},
        {"asia_markets", "Asia Markets"},
        {"ma_analytics", "M&A Analytics"},
        {"excel", "Excel"},
        {"data_mapping", "Data Mapping"},
        {"file_manager", "File Manager"},
        {"notes", "Notes"},
        {"docs", "Docs"},
        {"about", "About"},
        {"profile", "Profile"},
        {"settings", "Settings"},
        {"contact", "Contact"},
        {"terms", "Terms"},
        {"privacy", "Privacy"},
        {"trademarks", "Trademarks"},
        {"help", "Help"},
    };
    return titles.value(id, id);
}

DockScreenRouter::DockScreenRouter(ads::CDockManager* manager, QObject* parent) : QObject(parent), manager_(manager) {
}

void DockScreenRouter::register_screen(const QString& id, QWidget* screen) {
    screens_[id] = screen;
    factories_.remove(id);
}

void DockScreenRouter::register_factory(const QString& id, ScreenFactory factory) {
    factories_[id] = std::move(factory);
}

void DockScreenRouter::navigate(const QString& id, bool exclusive) {
    LOG_INFO("DockRouter",
             QString(">>> navigate('%1', exclusive=%2) panel_count=%3").arg(id).arg(exclusive).arg(panel_count_));
    auto* dw = find_dock_widget(id);

    bool needs_add = false;
    if (!dw) {
        if (!factories_.contains(id) && !screens_.contains(id)) {
            LOG_WARN("DockRouter", "Unknown screen: " + id);
            return;
        }
        dw = create_dock_widget(id);
        needs_add = true;
    } else if (!dw->dockManager()) {
        needs_add = true;
    }

    if (screens_.contains(id) && dw->widget() != screens_[id]) {
        QWidget* old = dw->widget();
        dw->setWidget(screens_[id]);
        if (old) {
            old->setParent(nullptr);
            old->deleteLater();
        }
    }

    materialize_screen(id);

    if (exclusive) {
        for (auto* area : manager_->openedDockAreas()) {
            for (auto* other : area->openedDockWidgets()) {
                if (other != dw)
                    other->toggleView(false);
            }
        }
        for (auto* other : manager_->dockWidgetsMap()) {
            if (other != dw && other->isAutoHide())
                other->toggleView(false);
        }
        grid_top_left_ = grid_top_right_ = grid_bottom_left_ = grid_bottom_right_ = nullptr;
        panel_count_ = 0;
    }

    auto* existing_area = dw->dockAreaWidget();
    const bool area_is_poisoned = existing_area && existing_area->dockWidgets().size() > 1;
    const bool must_place = needs_add || !existing_area || area_is_poisoned;

    auto sync_grid_from_reality = [this]() {
        const auto opened = manager_->openedDockAreas();
        panel_count_ = std::min(static_cast<int>(opened.size()), 4);

        if (grid_top_left_ && !opened.contains(grid_top_left_))
            grid_top_left_ = nullptr;
        if (grid_top_right_ && !opened.contains(grid_top_right_))
            grid_top_right_ = nullptr;
        if (grid_bottom_left_ && !opened.contains(grid_bottom_left_))
            grid_bottom_left_ = nullptr;
        if (grid_bottom_right_ && !opened.contains(grid_bottom_right_))
            grid_bottom_right_ = nullptr;

        int idx = 0;
        if (!grid_top_left_ && idx < opened.size())
            grid_top_left_ = opened.at(idx);
        if (grid_top_left_)
            idx = opened.indexOf(grid_top_left_) + 1;
        if (!grid_top_right_ && idx < opened.size())
            grid_top_right_ = opened.at(idx);
        if (grid_top_right_)
            idx = opened.indexOf(grid_top_right_) + 1;
        if (!grid_bottom_left_ && idx < opened.size())
            grid_bottom_left_ = opened.at(idx);
        if (grid_bottom_left_)
            idx = opened.indexOf(grid_bottom_left_) + 1;
        if (!grid_bottom_right_ && idx < opened.size())
            grid_bottom_right_ = opened.at(idx);

        LOG_INFO("DockRouter", QString("GRID synced: panel_count=%1 opened=%2 TL=%3 TR=%4 BL=%5 BR=%6")
                                   .arg(panel_count_)
                                   .arg(opened.size())
                                   .arg(grid_top_left_ ? "ok" : "-")
                                   .arg(grid_top_right_ ? "ok" : "-")
                                   .arg(grid_bottom_left_ ? "ok" : "-")
                                   .arg(grid_bottom_right_ ? "ok" : "-"));
    };

    if (exclusive) {
        auto* area = manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
        grid_top_left_ = area;
        grid_top_right_ = grid_bottom_left_ = grid_bottom_right_ = nullptr;
        panel_count_ = 1;

    } else if (must_place) {
        sync_grid_from_reality();

        LOG_INFO("DockRouter", QString("GRID [%1] must_place panel_count=%2").arg(id).arg(panel_count_));

        if (panel_count_ == 0) {
            LOG_INFO("DockRouter", QString("GRID [%1] -> PANEL 1 (full)").arg(id));
            auto* area = manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
            grid_top_left_ = area;
            panel_count_ = 1;

        } else if (panel_count_ == 1 && grid_top_left_) {
            LOG_INFO("DockRouter", QString("GRID [%1] -> PANEL 2 (right of TL)").arg(id));
            auto* area = manager_->addDockWidget(ads::RightDockWidgetArea, dw, grid_top_left_);
            grid_top_right_ = area;
            panel_count_ = 2;

        } else if (panel_count_ == 2 && grid_top_left_) {
            LOG_INFO("DockRouter", QString("GRID [%1] -> PANEL 3 (below TL)").arg(id));
            auto* area = manager_->addDockWidget(ads::BottomDockWidgetArea, dw, grid_top_left_);
            grid_bottom_left_ = area;
            panel_count_ = 3;

        } else if (panel_count_ == 3 && grid_bottom_left_) {
            LOG_INFO("DockRouter", QString("GRID [%1] -> PANEL 4 (right of BL)").arg(id));
            auto* area = manager_->addDockWidget(ads::RightDockWidgetArea, dw, grid_bottom_left_);
            grid_bottom_right_ = area;
            panel_count_ = 4;

        } else {
            LOG_INFO("DockRouter", QString("GRID [%1] -> tab into last (panel_count=%2)").arg(id).arg(panel_count_));
            ads::CDockAreaWidget* target =
                grid_bottom_right_
                    ? grid_bottom_right_
                    : (manager_->openedDockAreas().isEmpty() ? nullptr : manager_->openedDockAreas().last());
            if (target)
                manager_->addDockWidget(ads::CenterDockWidgetArea, dw, target);
            else
                manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
            panel_count_++;
        }

    } else {
        LOG_INFO("DockRouter", QString("GRID [%1] already placed, syncing grid").arg(id));
        sync_grid_from_reality();
    }

    dw->toggleView(true);
    if (auto* tab = dw->tabWidget()) {
        if (tab->isHidden())
            tab->setVisible(true);
    }
    dw->raise();
    dw->setAsCurrentTab();

    apply_ads_theme();

    current_id_ = id;
    SessionManager::instance().set_last_screen(id);
    LOG_INFO("DockRouter", "Navigated to: " + id);
    emit screen_changed(id);
}

void DockScreenRouter::tab_into(const QString& id) {
    if (!factories_.contains(id) && !screens_.contains(id)) {
        LOG_WARN("DockRouter", "Unknown screen: " + id);
        return;
    }

    auto* dw = find_dock_widget(id);
    bool needs_add = !dw || !dw->dockManager();

    if (!dw) {
        dw = create_dock_widget(id);
        needs_add = true;
    }

    materialize_screen(id);

    if (!dw->isClosed() && dw->dockAreaWidget()) {
        dw->raise();
        dw->setAsCurrentTab();
        current_id_ = id;
        emit screen_changed(id);
        return;
    }

    ads::CDockAreaWidget* target_area = nullptr;
    if (auto* focused = manager_->focusedDockWidget())
        target_area = focused->dockAreaWidget();
    if (!target_area) {
        const auto opened = manager_->openedDockAreas();
        if (!opened.isEmpty())
            target_area = opened.first();
    }

    if (needs_add || !dw->dockAreaWidget()) {
        if (target_area)
            manager_->addDockWidget(ads::CenterDockWidgetArea, dw, target_area);
        else
            manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
    }

    dw->toggleView(true);
    if (auto* tab = dw->tabWidget()) {
        if (tab->isHidden())
            tab->setVisible(true);
    }
    dw->raise();
    dw->setAsCurrentTab();

    current_id_ = id;
    SessionManager::instance().set_last_screen(id);
    LOG_INFO("DockRouter", "Tabbed into: " + id);
    emit screen_changed(id);
}

void DockScreenRouter::add_alongside(const QString& primary, const QString& secondary) {
    auto* primary_dw = find_dock_widget(primary);
    bool primary_visible = primary_dw && !primary_dw->isClosed() && primary_dw->dockAreaWidget();

    LOG_INFO("DockRouter", QString("add_alongside: primary=%1 visible=%2 secondary=%3 panel_count=%4")
                               .arg(primary)
                               .arg(primary_visible)
                               .arg(secondary)
                               .arg(panel_count_));

    if (primary_visible) {
        navigate(secondary, false);
    } else {
        navigate(primary, true);
        navigate(secondary, false);
    }

    primary_dw = find_dock_widget(primary);
    if (primary_dw && !primary_dw->isClosed()) {
        primary_dw->raise();
        primary_dw->setAsCurrentTab();
        current_id_ = primary;
        emit screen_changed(primary);
    }
}

void DockScreenRouter::remove_screen(const QString& primary) {
    auto* keep = find_dock_widget(primary);
    for (auto* dw : manager_->dockWidgetsMap()) {
        if (dw != keep && !dw->isClosed())
            dw->toggleView(false);
    }
    if (keep) {
        if (keep->isClosed())
            keep->toggleView(true);
        keep->raise();
        keep->setAsCurrentTab();
        current_id_ = primary;
        emit screen_changed(primary);
    }
}

void DockScreenRouter::replace_screen(const QString& primary, const QString& secondary) {
    Q_UNUSED(primary)
    navigate(secondary, true);
}

void DockScreenRouter::ensure_all_registered() {
    QStringList all_ids;
    for (auto it = factories_.cbegin(); it != factories_.cend(); ++it)
        all_ids.append(it.key());
    for (auto it = screens_.cbegin(); it != screens_.cend(); ++it) {
        if (!all_ids.contains(it.key()))
            all_ids.append(it.key());
    }

    ads::CDockAreaWidget* first_area = nullptr;
    for (const QString& id : all_ids) {
        if (!dock_widgets_.contains(id)) {
            auto* dw = create_dock_widget(id);
            if (first_area)
                manager_->addDockWidget(ads::CenterDockWidgetArea, dw, first_area);
            else
                first_area = manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
            dw->toggleView(false);
        }
    }
    apply_ads_theme();
}

ads::CDockWidget* DockScreenRouter::find_dock_widget(const QString& id) const {
    return dock_widgets_.value(id, nullptr);
}

QStringList DockScreenRouter::all_screen_ids() const {
    QStringList ids;
    ids.reserve(dock_widgets_.size() + factories_.size());
    for (auto it = dock_widgets_.cbegin(); it != dock_widgets_.cend(); ++it)
        ids.append(it.key());
    for (auto it = factories_.cbegin(); it != factories_.cend(); ++it) {
        if (!ids.contains(it.key()))
            ids.append(it.key());
    }
    return ids;
}

bool DockScreenRouter::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() != QEvent::MouseButtonDblClick)
        return QObject::eventFilter(obj, event);

    auto* lbl = qobject_cast<QLabel*>(obj);
    if (!lbl)
        return QObject::eventFilter(obj, event);

    const QString id = lbl->property("dock_id").toString();
    auto* dw = dock_widgets_.value(id, nullptr);
    if (!dw)
        return QObject::eventFilter(obj, event);

    QWidget* tab_frame = lbl->parentWidget();
    auto* editor = new QLineEdit(tab_frame);
    editor->setText(dw->windowTitle());
    editor->selectAll();
    editor->setFrame(false);
    editor->setStyleSheet("QLineEdit{"
                          "  background:#1a1a1a;"
                          "  color:#ffffff;"
                          "  border:1px solid #d97706;"
                          "  padding:0 4px;"
                          "  font-size:11px;"
                          "  font-family:'Consolas',monospace;"
                          "}");
    editor->setGeometry(tab_frame->mapFromGlobal(lbl->mapToGlobal(QPoint(0, 0))).x(), 2, lbl->width(),
                        tab_frame->height() - 4);
    editor->show();
    editor->raise();
    editor->setFocus();

    auto commit = [dw, editor]() {
        const QString name = editor->text().trimmed();
        if (!name.isEmpty()) {
            dw->setWindowTitle(name);
        }
        editor->deleteLater();
    };

    connect(editor, &QLineEdit::returnPressed, editor, commit);
    connect(editor, &QLineEdit::editingFinished, editor, commit);

    struct EscFilter : QObject {
        QLineEdit* ed;
        explicit EscFilter(QLineEdit* e) : QObject(e), ed(e) {}
        bool eventFilter(QObject*, QEvent* e) override {
            if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
                ed->deleteLater();
                return true;
            }
            return false;
        }
    };
    editor->installEventFilter(new EscFilter(editor));

    return true;
}

ads::CDockWidget* DockScreenRouter::create_dock_widget(const QString& id) {
    if (dock_widgets_.contains(id))
        return dock_widgets_[id];

    auto* dw = new ads::CDockWidget(title_for_id(id));
    dw->setObjectName(id);
    dw->setFeatures(ads::CDockWidget::DockWidgetMovable | ads::CDockWidget::DockWidgetFloatable |
                    ads::CDockWidget::DockWidgetClosable | ads::CDockWidget::DockWidgetFocusable);
    dw->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromDockWidget);
    dw->setMinimumWidth(200);

    if (screens_.contains(id)) {
        screens_[id]->setMinimumWidth(0);
        dw->setWidget(screens_[id]);
    } else {
        auto* placeholder = new QWidget;
        auto* lbl = new QLabel(title_for_id(id));
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet("color:#555;font-size:16px;");
        auto* vl = new QVBoxLayout(placeholder);
        vl->addWidget(lbl);
        dw->setWidget(placeholder);
    }

    if (auto* tab = dw->tabWidget()) {
        if (auto* lbl = tab->findChild<QLabel*>("dockWidgetTabLabel")) {
            lbl->installEventFilter(this);
            lbl->setProperty("dock_id", id);
        }
    }

    const QString saved_title = load_tab_title(id);
    if (!saved_title.isEmpty())
        dw->setWindowTitle(saved_title);

    connect(dw, &ads::CDockWidget::titleChanged, this, [this, id](const QString& title) {
        if (title != title_for_id(id))
            save_tab_title(id, title);
    });

    connect(dw, &ads::CDockWidget::visibilityChanged, this, [this, id](bool visible) {
        if (visible) {
            materialize_screen(id);
        } else {
            save_screen_state(id);
        }
    });

    dock_widgets_[id] = dw;
    return dw;
}

void DockScreenRouter::materialize_screen(const QString& id) {
    if (screens_.contains(id))
        return;

    auto fit = factories_.find(id);
    if (fit == factories_.end())
        return;

    LOG_INFO("DockRouter", "Lazy-constructing screen: " + id);
    QWidget* screen = nullptr;
    try {
        screen = fit.value()();
    } catch (const std::exception& e) {
        LOG_ERROR("DockRouter", QString("Factory threw for '%1': %2").arg(id, e.what()));
    } catch (...) {
        LOG_ERROR("DockRouter", QString("Factory threw unknown exception for '%1'").arg(id));
    }
    if (!screen) {
        LOG_ERROR("DockRouter", "Factory returned null for: " + id);
        return;
    }
    screen->setMinimumWidth(0);
    factories_.erase(fit);

    auto* dw = dock_widgets_.value(id, nullptr);
    if (!dw)
        return;

    QWidget* old = dw->widget();
    dw->setWidget(screen);
    if (old) {
        old->setParent(nullptr);
        old->deleteLater();
    }
    screens_[id] = screen;
    restore_screen_state(id);
}

void DockScreenRouter::save_tab_title(const QString& id, const QString& title) {
    SessionManager::instance().save_tab_state("title/" + id, {{"title", title}});
}

QString DockScreenRouter::load_tab_title(const QString& id) const {
    const auto state = SessionManager::instance().load_tab_state("title/" + id);
    return state.value("title").toString();
}

void DockScreenRouter::save_screen_state(const QString& id) {
    auto* screen = screens_.value(id, nullptr);
    if (!screen)
        return;
    auto* stateful = dynamic_cast<screens::IStatefulScreen*>(screen);
    if (!stateful)
        return;
    ScreenStateManager::instance().save_now(stateful);
}

void DockScreenRouter::restore_screen_state(const QString& id) {
    auto* screen = screens_.value(id, nullptr);
    if (!screen)
        return;
    auto* stateful = dynamic_cast<screens::IStatefulScreen*>(screen);
    if (!stateful)
        return;
    ScreenStateManager::instance().restore(stateful);
}

void DockScreenRouter::apply_ads_theme() {
    for (auto* dw : manager_->dockWidgetsMap()) {
        if (auto* tab = dw->tabWidget()) {
            if (!tab->styleSheet().isEmpty())
                tab->setStyleSheet(QString());
            if (tab->isHidden() && !dw->isClosed())
                tab->setVisible(true);
            for (auto* lbl : tab->findChildren<QLabel*>()) {
                if (!lbl->styleSheet().isEmpty())
                    lbl->setStyleSheet(QString());
            }
        }
    }
}

} // namespace fincept
