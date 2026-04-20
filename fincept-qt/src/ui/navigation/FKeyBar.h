#pragma once
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QWidget>

namespace fincept::ui {

/// Scrollable tab bar for the personal local terminal build.
/// Core tabs only: Dashboard, Markets, Crypto, Portfolio, News,
/// AI Chat, Backtesting, Algo, Nodes, Code, Settings, Profile.
class TabBar : public QWidget {
    Q_OBJECT
  public:
    explicit TabBar(QWidget* parent = nullptr);

    void set_active(const QString& tab_id);
    QString active_tab() const { return active_id_; }

  signals:
    void tab_changed(const QString& tab_id);

  private:
    struct TabDef {
        QString id;
        QString label;
    };

    QHBoxLayout* tab_layout_ = nullptr;
    QVector<QPushButton*> tab_buttons_;
    QString active_id_ = "dashboard";

    void add_tab(const TabDef& def);
    void refresh_theme();
    void update_styles();
};

} // namespace fincept::ui
