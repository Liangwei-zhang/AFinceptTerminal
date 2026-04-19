#include "ai_chat/LlmService.h"
#include "app/MainWindow.h"
#include "core/config/AppConfig.h"
#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "core/keys/KeyConfigManager.h"
#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "core/session/SessionManager.h"
#include "datahub/DataHubMetaTypes.h"
#include "mcp/McpInit.h"
#include "network/http/HttpClient.h"
#include "python/PythonSetupManager.h"
#include "screens/setup/SetupScreen.h"
#include "services/markets/MarketDataService.h"
#include "services/news/NewsService.h"
#include "storage/repositories/NewsArticleRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/sqlite/CacheDatabase.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <singleapplication.h>

#ifdef Q_OS_WIN
#    include <Windows.h>
#endif

int main(int argc, char* argv[]) {
    {
        for (int i = 1; i < argc - 1; ++i) {
            if (qstrcmp(argv[i], "--profile") == 0) {
                fincept::ProfileManager::instance().set_active(QString::fromUtf8(argv[i + 1]));
                break;
            }
        }
        QDir().mkpath(fincept::AppPaths::root());
    }

    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    const QString profile_key = QString("FinceptTerminal-%1").arg(fincept::ProfileManager::instance().active());
    SingleApplication app(argc, argv, /*allowSecondary=*/true, SingleApplication::Mode::User, 100,
                          profile_key.toUtf8());
    app.setApplicationName("FinceptTerminal");
    app.setOrganizationName("Fincept");
#ifndef FINCEPT_VERSION_STRING
#    define FINCEPT_VERSION_STRING "0.0.0-dev"
#endif
    app.setApplicationVersion(QStringLiteral(FINCEPT_VERSION_STRING));

    if (app.isSecondary()) {
#ifdef Q_OS_WIN
        AllowSetForegroundWindow(static_cast<DWORD>(app.primaryPid()));
#endif
        app.sendMessage(QByteArray("--new-window"));
        return 0;
    }

    // Local desktop mode: keep only the core producers needed by the
    // streamlined Nasdaq workflow.
    fincept::datahub::register_metatypes();
    fincept::services::MarketDataService::instance().ensure_registered_with_hub();
    fincept::services::NewsService::instance().ensure_registered_with_hub();

    fincept::AppPaths::ensure_all();

    {
        const QString old_base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const auto migrate_file = [](const QString& old_path, const QString& new_path) {
            if (QFile::exists(old_path) && !QFile::exists(new_path))
                QFile::rename(old_path, new_path);
        };
        migrate_file(old_base + "/fincept.db", fincept::AppPaths::data() + "/fincept.db");
        migrate_file(old_base + "/cache.db", fincept::AppPaths::data() + "/cache.db");
        migrate_file(old_base + "/fincept.log", fincept::AppPaths::logs() + "/fincept.log");
        migrate_file(old_base + "/fincept-files", fincept::AppPaths::files());
        QFile::remove(old_base + "/fincept.db-wal");
        QFile::remove(old_base + "/fincept.db-shm");
        QFile::remove(old_base + "/cache.db-wal");
        QFile::remove(old_base + "/cache.db-shm");
    }

    QLockFile db_lock(fincept::AppPaths::data() + "/fincept.db.lock");
    db_lock.setStaleLockTime(0);
    const bool sole_instance = db_lock.tryLock(0);
    if (sole_instance) {
        QFile::remove(fincept::AppPaths::data() + "/fincept.db-wal");
        QFile::remove(fincept::AppPaths::data() + "/fincept.db-shm");
        QFile::remove(fincept::AppPaths::data() + "/cache.db-wal");
        QFile::remove(fincept::AppPaths::data() + "/cache.db-shm");
    }
    {
        const QString local_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        const QString legacy1 = local_dir.section('/', 0, -3) + "/FinceptTerminal/fincept_settings.db";
        const QString legacy2 =
            QString(local_dir).replace("Fincept/FinceptTerminal", "FinceptTerminal") + "/fincept_settings.db";
        QFile::remove(legacy1 + "-wal");
        QFile::remove(legacy1 + "-shm");
        QFile::remove(legacy2 + "-wal");
        QFile::remove(legacy2 + "-shm");
    }

    fincept::Logger::instance().set_file(fincept::AppPaths::logs() + "/fincept.log");
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
        const char* category = (ctx.category && *ctx.category) ? ctx.category : "Qt";
        switch (type) {
        case QtDebugMsg:    fincept::Logger::instance().debug(category, msg); break;
        case QtInfoMsg:     fincept::Logger::instance().info(category, msg); break;
        case QtWarningMsg:  fincept::Logger::instance().warn(category, msg); break;
        case QtCriticalMsg: fincept::Logger::instance().error(category, msg); break;
        case QtFatalMsg:
            fincept::Logger::instance().error(category, msg);
            fincept::Logger::instance().flush_and_close();
            break;
        }
    });
    {
        auto& log = fincept::Logger::instance();
        auto& cfg = fincept::AppConfig::instance();
        const QString gl = cfg.get("log/global_level", "Info").toString();
        const QHash<QString, fincept::LogLevel> lvl_map = {{"Trace", fincept::LogLevel::Trace},
                                                           {"Debug", fincept::LogLevel::Debug},
                                                           {"Info", fincept::LogLevel::Info},
                                                           {"Warn", fincept::LogLevel::Warn},
                                                           {"Error", fincept::LogLevel::Error},
                                                           {"Fatal", fincept::LogLevel::Fatal}};
        log.set_level(lvl_map.value(gl, fincept::LogLevel::Info));
        log.set_json_mode(cfg.get("log/json_mode", false).toBool());
        const int count = cfg.get("log/tag_count", 0).toInt();
        for (int i = 0; i < count; ++i) {
            const QString tag = cfg.get(QString("log/tag_%1_name").arg(i)).toString();
            const QString level = cfg.get(QString("log/tag_%1_level").arg(i)).toString();
            if (!tag.isEmpty() && lvl_map.contains(level))
                log.set_tag_level(tag, lvl_map.value(level));
        }
    }
    LOG_INFO("App", "Fincept Terminal local mode starting...");

    auto& config = fincept::AppConfig::instance();
    fincept::HttpClient::instance().set_base_url(config.api_base_url());

    fincept::register_migration_v001();
    fincept::register_migration_v002();
    fincept::register_migration_v003();
    fincept::register_migration_v004();
    fincept::register_migration_v005();
    fincept::register_migration_v006();
    fincept::register_migration_v007();
    fincept::register_migration_v008();
    fincept::register_migration_v009();
    fincept::register_migration_v010();
    fincept::register_migration_v011();
    fincept::register_migration_v012();
    fincept::register_migration_v013();
    fincept::register_migration_v014();
    fincept::register_migration_v015();
    fincept::register_migration_v016();
    fincept::register_migration_v017();
    fincept::register_migration_v018();

    const QString db_path = fincept::AppPaths::data() + "/fincept.db";
    auto db_result = fincept::Database::instance().open(db_path);
    if (db_result.is_err()) {
        LOG_ERROR("App", "Failed to open database: " + QString::fromStdString(db_result.error()));
        fincept::ui::apply_global_stylesheet();
    } else {
        const int64_t news_cutoff = QDateTime::currentSecsSinceEpoch() - (30LL * 86400);
        QTimer::singleShot(0, [news_cutoff]() {
            fincept::NewsArticleRepository::instance().prune_older_than(news_cutoff);
            LOG_INFO("App", "News articles pruned (keeping 30 days)");
        });

        auto& repo = fincept::SettingsRepository::instance();
        auto& tm = fincept::ui::ThemeManager::instance();
        auto r_family = repo.get("appearance.font_family");
        auto r_size = repo.get("appearance.font_size");
        QString family = r_family.is_ok() ? r_family.value() : "Consolas";
        QString size_s = r_size.is_ok() ? r_size.value() : "14px";
        int size_px = size_s.left(size_s.indexOf("px")).toInt();
        if (size_px <= 0)
            size_px = 14;
        tm.apply_font(family, size_px);
        tm.apply_theme("Obsidian");
        LOG_INFO("App", "Theme: Obsidian, font: " + family + " " + size_s);
    }

    const QString cache_path = fincept::AppPaths::data() + "/cache.db";
    auto cache_result = fincept::CacheDatabase::instance().open(cache_path);
    if (cache_result.is_err())
        LOG_WARN("App", "Cache DB failed (non-fatal): " + QString::fromStdString(cache_result.error()));

    {
        const QString sid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        fincept::ScreenStateManager::instance().set_session_id(sid);
        LOG_INFO("App", "Session ID: " + sid);
    }

    LOG_INFO("App", "Checking settings for legacy migration...");
    {
        auto existing = fincept::SettingsRepository::instance().get("fincept_session");
        const bool new_db_empty = existing.is_err() || existing.value().isEmpty();
        if (new_db_empty) {
            QString local_base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            QString old_db_path = local_base.section('/', 0, -3) + "/FinceptTerminal/fincept_settings.db";
            if (!QFile::exists(old_db_path))
                old_db_path = local_base.replace("Fincept/FinceptTerminal", "FinceptTerminal") + "/fincept_settings.db";
            if (QFile::exists(old_db_path)) {
                QSqlDatabase old_db = QSqlDatabase::addDatabase("QSQLITE", "legacy_migration");
                old_db.setDatabaseName(old_db_path);
                if (old_db.open()) {
                    QSqlQuery src(old_db);
                    if (src.exec("SELECT key, value FROM settings")) {
                        int count = 0;
                        while (src.next()) {
                            fincept::SettingsRepository::instance().set(src.value(0).toString(),
                                                                        src.value(1).toString(), "migrated");
                            ++count;
                        }
                        LOG_INFO("App", QString("Migrated %1 settings from legacy DB").arg(count));
                    }
                    old_db.close();
                }
                QSqlDatabase::removeDatabase("legacy_migration");
            }
        }
    }

    fincept::SessionManager::instance().start_session();

    // Keep MCP optional for local mode. Tools still work where configured,
    // but there is no login or account gate anymore.
    fincept::mcp::initialize_all_tools();

    auto setup_status = fincept::python::PythonSetupManager::instance().check_status();

    if (setup_status.needs_setup) {
        LOG_INFO("App", "Python environment not ready — showing setup screen");
        auto* setup_screen = new fincept::screens::SetupScreen;
        QPointer<fincept::screens::SetupScreen> screen_guard(setup_screen);
        setup_screen->setWindowTitle("Fincept Terminal — First-Time Setup");
        setup_screen->resize(800, 600);
        setup_screen->show();

        QObject::connect(setup_screen, &fincept::screens::SetupScreen::setup_complete,
                         [&app, screen_guard]() {
            if (!screen_guard)
                return;
            screen_guard->hide();
            screen_guard->deleteLater();

            fincept::KeyConfigManager::instance();
            auto* window = new fincept::MainWindow(0);
            window->setAttribute(Qt::WA_DeleteOnClose);
            window->show();

            QObject::connect(&app, &SingleApplication::receivedMessage,
                             [](quint32, QByteArray) {
                                 auto* w = new fincept::MainWindow(fincept::MainWindow::next_window_id());
                                 w->setAttribute(Qt::WA_DeleteOnClose);
                                 w->show();
                                 w->raise();
                                 w->activateWindow();
                                 LOG_INFO("App", "New window opened via secondary instance request");
                             });
            QObject::connect(&app, &QApplication::lastWindowClosed, &app, &QApplication::quit);

            if (!fincept::ai_chat::LlmService::instance().is_configured())
                LOG_WARN("App", "LLM provider not configured — AI chat will prompt user to configure Settings → LLM Config");
            LOG_INFO("App", "Application ready (after setup)");
        });

        return app.exec();
    }

    fincept::KeyConfigManager::instance();

    fincept::MainWindow window;
    window.show();

    QObject::connect(&app, &SingleApplication::receivedMessage, [](quint32, QByteArray) {
        auto* w = new fincept::MainWindow(fincept::MainWindow::next_window_id());
        w->setAttribute(Qt::WA_DeleteOnClose);
        w->show();
        w->raise();
        w->activateWindow();
        LOG_INFO("App", "New window opened via secondary instance request");
    });
    QObject::connect(&app, &QApplication::lastWindowClosed, &app, &QApplication::quit);

    if (setup_status.needs_package_sync) {
        LOG_INFO("App", "Requirements changed — syncing packages in background");
        auto& mgr = fincept::python::PythonSetupManager::instance();
        QObject::connect(&mgr, &fincept::python::PythonSetupManager::setup_complete,
                         &mgr, [](bool success, const QString& error) {
            if (success)
                LOG_INFO("App", "Background package sync completed successfully");
            else
                LOG_WARN("App", "Background package sync failed (non-fatal): " + error);
        }, Qt::SingleShotConnection);
        mgr.run_setup();
    }

    if (!fincept::ai_chat::LlmService::instance().is_configured())
        LOG_WARN("App", "LLM provider not configured — AI chat will prompt user to configure Settings → LLM Config");
    LOG_INFO("App", "Application ready");
    return app.exec();
}
