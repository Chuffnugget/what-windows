#include "app/Controller.h"
#include <QJsonDocument>
#include <QUuid>

void Controller::initCustomSources() {
    customSources = new CustomModule(testMode, this);
    connect(customSources, &CustomModule::diagnostic, this, &Controller::log);
    connect(customSources, &CustomModule::changed, this, &Controller::changed);
    customSources->restore(QJsonArray::fromVariantList(settings.value("custom/definitions").toList()),
                           settings.value("modules/custom/enabled", false).toBool(),
                           settings.value("modules/custom/control", false).toBool(),
                           settings.value("modules/custom/cloud", false).toBool());
    connect(customSources, &CustomModule::configurationChanged, this, [this] {
        settings.setValue("custom/definitions", customSources->definitions().toVariantList());
        settings.setValue("modules/custom/enabled", customSources->enabled());
        settings.setValue("modules/custom/control", customSources->controlEnabled());
        settings.setValue("modules/custom/cloud", customSources->cloudEnabled());
        scheduleSettingsFlush();
        publishPolicy();
    });
    connect(customSources, &CustomModule::readingsChanged, this, [this] {
        ++customGeneration;
        customRefreshTimer.start();
        emit changed();
    });
    connect(customSources, &CustomModule::actionFinished, this,
            [this](const QString &request, bool ok, const QString &message) {
                const auto separator = request.indexOf('\n');
                if (separator >= 0)
                    server.replyCommand(request.left(separator), request.mid(separator + 1), ok, message);
                else
                    emit customActionFinished(ok, message);
                log(QString("Custom action %1").arg(ok ? "completed" : "failed; inspect its status"));
            });
    customRefreshTimer.setInterval(50);
    customRefreshTimer.setSingleShot(true);
    connect(&customRefreshTimer, &QTimer::timeout, this, &Controller::refreshCustomReadings);
    customCatalogJob.onStatus = [this](const QString &error) {
        if (!error.isEmpty()) {
            log("Custom catalogue preparation failed");
            customRefreshTimer.start();
        }
    };
    customRefreshTimer.start();
}

void Controller::refreshCustomReadings() {
    if (customCatalogJob.isRunning()) {
        customRefreshTimer.start();
        return;
    }
    const auto build = customSources->captureReadings();
    const auto prepare = prepareReadingsUpdate("custom");
    const auto generation = customGeneration;
    customCatalogJob.submit([build, prepare] { return prepare(build()); },
                            [this, generation](ReadingsUpdate update) {
                                if (generation != customGeneration) {
                                    customRefreshTimer.start();
                                    return;
                                }
                                applyReadingsUpdate("custom", std::move(update));
                                publish();
                                emit sensorsChanged();
                                emit changed();
                            });
}

void Controller::runCustomAction(const QString &id) {
    customSources->runAction(id, QUuid::createUuid().toString(QUuid::Id128));
}

void Controller::handleCustomAction(const QString &session, const QJsonObject &command) {
    const auto args = command["args"].toObject();
    if (args.size() != 1 || !args["id"].isString()) {
        server.replyCommand(session, command["id"].toString(), false, "Expected a saved command ID");
        return;
    }
    const auto lease = server.commandLease(session);
    if (lease && lease->load())
        customSources->runAction(args["id"].toString(), session + '\n' + command["id"].toString(), lease,
                                 command["_custom_cloud"].toBool());
}
