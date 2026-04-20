#include "ui/navigation/NavigationBar.h"

#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QDateTime>

namespace fincept::ui {

NavigationBar::NavigationBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(38);
    setObjectName("navBar");

    auto* hl = new QHBoxLayout(this);
    hl->setContentsMargins(14, 0, 14, 0);
    hl->setSpacing(0);

    auto mk = [](const QString& t, const QString& name) {
        auto* l = new QLabel(t);
        l->setObjectName(name);
        return l;
    };

    hl->addWidget(mk("FINCEPT", "navBrand"));
    hl->addWidget(mk("TERMINAL", "navTitle"));
    hl->addWidget(mk("   ", "navSpacer"));
    hl->addWidget(mk("\xe2\x97\x8f", "navLiveDot"));
    hl->addWidget(mk(" LIVE", "navLive"));
    hl->addStretch();
    clock_label_ = mk("", "navClock");
    hl->addWidget(clock_label_);
    hl->addStretch();
    user_label_ = mk("LOCAL MODE", "navUser");
    hl->addWidget(user_label_);
    hl->addWidget(mk("  |  ", "navSep"));
    credits_label_ = mk("OFFLINE", "navCredits");
    hl->addWidget(credits_label_);
    hl->addWidget(mk("  |  ", "navSep"));
    plan_label_ = mk("SINGLE USER", "navPlan");
    hl->addWidget(plan_label_);

    if (logout_btn_) {
        logout_btn_->hide();
    }

    clock_timer_ = new QTimer(this);
    clock_timer_->setInterval(1000);
    connect(clock_timer_, &QTimer::timeout, this, &NavigationBar::update_clock);
    clock_timer_->start();
    update_clock();

    connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
            [this](const ThemeTokens&) { refresh_theme(); });

    refresh_user_display();
    refresh_theme();
}

void NavigationBar::refresh_theme() {
    setStyleSheet(QString("#navBar { background:%1; border-bottom:1px solid %2; }"
                          "#navBrand { color:%3; font-weight:700; background:transparent; }"
                          "#navTitle { color:%4; background:transparent; }"
                          "#navSpacer { background:transparent; }"
                          "#navLiveDot { color:%5; background:transparent; }"
                          "#navLive { color:%5; font-weight:700; background:transparent; }"
                          "#navClock { color:%6; background:transparent; }"
                          "#navUser { color:%3; background:transparent; }"
                          "#navSep { color:%2; background:transparent; }"
                          "#navCredits { color:%5; background:transparent; }"
                          "#navPlan { color:%4; background:transparent; }")
                      .arg(colors::BG_BASE())       // %1
                      .arg(colors::BORDER_DIM())    // %2
                      .arg(colors::AMBER())         // %3
                      .arg(colors::TEXT_PRIMARY())  // %4
                      .arg(colors::POSITIVE())      // %5
                      .arg(colors::TEXT_TERTIARY())); // %6
}

void NavigationBar::update_clock() {
    clock_label_->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd  HH:mm:ss"));
}

void NavigationBar::refresh_user_display() {
    user_label_->setText("LOCAL MODE");
    credits_label_->setText("OFFLINE");
    credits_label_->setStyleSheet(QString("color:%1;background:transparent;").arg(colors::POSITIVE.get()));
    plan_label_->setText("SINGLE USER");
}

} // namespace fincept::ui
