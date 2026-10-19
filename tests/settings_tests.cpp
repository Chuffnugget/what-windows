#include "app/SettingsStore.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <thread>

class SettingsTests : public QObject {
    Q_OBJECT

    static QByteArray contents(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    static void write(const QString &path, const QByteArray &bytes) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }

private slots:

    void valuesSurviveDiskRoundTrip() {
        QTemporaryDir dir;
        const auto path = SettingsStore::profilePath(dir.path());
        const QByteArray binary = QByteArray::fromHex("0001027fff00").repeated(20000);
        const QStringList ids{QString::fromUtf8("CPU/温度"), "false", "123", "line\nbreak",
                              QString::fromUtf8("quote\" slash\\ tab\t\x7f\xc2\x9f\xc2\x85\xe2\x80\xa8\xe2\x80\xa9 end")};
        {
            QSettings settings(path, SettingsStore::format());
            settings.setValue("ui/window_mode", "fullscreen");
            settings.setValue("selected", ids);
            settings.setValue("mqtt/password", binary);
            settings.setValue("emptyBytes", QByteArray());
            settings.setValue("emptyList", QStringList());
            settings.setValue("emptyMap", QVariantMap());
            settings.setValue("sample_seconds", 0.5);
            settings.setValue("maximum", std::numeric_limits<quint64>::max());
            settings.setValue("mqtt/options",
                              QVariantMap{{"enabled", false}, {"port", 8883}, {"host", "off"}});
            settings.sync();
            QCOMPARE(settings.status(), QSettings::NoError);
        }
        // A different path avoids QSettings' shared in-process file cache.
        const auto reloadedPath = dir.filePath("reloaded.yaml");
        QVERIFY(QFile::copy(path, reloadedPath));
        QSettings reloaded(reloadedPath, SettingsStore::format());
        QCOMPARE(reloaded.status(), QSettings::NoError);
        QCOMPARE(reloaded.value("ui/window_mode").toString(), QString("fullscreen"));
        QCOMPARE(reloaded.value("selected").toStringList(), ids);
        QCOMPARE(reloaded.value("mqtt/password").toByteArray(), binary);
        QCOMPARE(reloaded.value("emptyBytes").typeId(), QMetaType::QByteArray);
        QVERIFY(reloaded.value("emptyList").toList().isEmpty());
        QCOMPARE(reloaded.value("emptyMap").typeId(), QMetaType::QVariantMap);
        QCOMPARE(reloaded.value("sample_seconds").toDouble(), 0.5);
        QCOMPARE(reloaded.value("maximum").toULongLong(), std::numeric_limits<quint64>::max());
        const auto broker = reloaded.value("mqtt/options").toMap();
        QCOMPARE(broker["enabled"].typeId(), QMetaType::Bool);
        QVERIFY(!broker["enabled"].toBool());
        QCOMPARE(broker["port"].toInt(), 8883);
        QCOMPARE(broker["host"].toString(), QString("off"));
        QVERIFY(contents(path).contains("!!binary"));
        QVERIFY(!QFile::exists(dir.filePath("settings.ini")));
    }

    void cachedEncodingTracksChangesAndTypes() {
        QTemporaryDir dir;
        const auto path = dir.filePath("settings.yaml");
        QSettings settings(path, SettingsStore::format());
        settings.setValue("unchanged", "keep me");
        settings.setValue("removed", 42);
        settings.setValue("scalar", "false");
        settings.setValue("nested", QVariantMap{{"items", QVariantList{1, "true"}}});
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);

        // Equal-looking scalar values still need new YAML when their types change.
        settings.setValue("scalar", false);
        settings.setValue("nested", QVariantMap{{"items", QVariantList{"1", true}}});
        settings.remove("removed");
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        const auto copy = dir.filePath("copy.yaml");
        QVERIFY(QFile::copy(path, copy));
        QSettings reloaded(copy, SettingsStore::format());
        QCOMPARE(reloaded.value("unchanged").toString(), QString("keep me"));
        QVERIFY(!reloaded.contains("removed"));
        QCOMPARE(reloaded.value("scalar").typeId(), QMetaType::Bool);
        QVERIFY(!reloaded.value("scalar").toBool());
        const auto items = reloaded.value("nested").toMap()["items"].toList();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items[0].typeId(), QMetaType::QString);
        QCOMPARE(items[0].toString(), QString("1"));
        QCOMPARE(items[1].typeId(), QMetaType::Bool);
        QVERIFY(items[1].toBool());

        settings.clear();
        settings.sync();
        QVERIFY(contents(path).endsWith("settings: {}\n"));
        settings.setValue("scalar", "false");
        settings.sync();
        const auto fresh = dir.filePath("fresh.yaml");
        QVERIFY(QFile::copy(path, fresh));
        QSettings restored(fresh, SettingsStore::format());
        QCOMPARE(restored.allKeys(), QStringList{"scalar"});
        QCOMPARE(restored.value("scalar").typeId(), QMetaType::QString);
        QCOMPARE(restored.value("scalar").toString(), QString("false"));
    }

    void migratesOnceAndDoesNotFallBack() {
        QTemporaryDir dir;
        const auto ini = dir.filePath("settings.ini");
        const auto binary = QByteArray::fromHex("007faabbff00");
        {
            QSettings old(ini, QSettings::IniFormat);
            old.setValue("device_id", "keep-device");
            old.setValue("token", binary);
            old.setValue("selected", QStringList{"cpu", "gpu"});
            old.setValue("displays/recovery", QByteArray("{\"layout\":[]}"));
            old.setValue("updates/automatic", false);
            old.sync();
        }
        const auto original = contents(ini);
        write(ini + ".import.yaml", "queued-import");
        const auto path = SettingsStore::profilePath(dir.path());
        QVERIFY(path.endsWith("settings.yaml"));
        QVERIFY(!QFile::exists(ini));
        QCOMPARE(contents(ini + ".migrated"), original);
        QCOMPARE(contents(path + ".import.yaml"), QByteArray("queued-import"));
        QVERIFY(!QFile::exists(ini + ".import.yaml"));
        {
            QSettings settings(path, SettingsStore::format());
            QCOMPARE(settings.value("device_id").toString(), QString("keep-device"));
            QCOMPARE(settings.value("token").toByteArray(), binary);
            QCOMPARE(settings.value("selected").toStringList(), (QStringList{"cpu", "gpu"}));
            QCOMPARE(settings.value("displays/recovery").toByteArray(), QByteArray("{\"layout\":[]}"));
            QVERIFY(!settings.value("updates/automatic").toBool());
        }
        // A recreated INI cannot replace the established YAML profile.
        write(ini, "[General]\ndevice_id=wrong-device\n");
        const auto yaml = contents(path);
        QCOMPARE(SettingsStore::profilePath(dir.path()), path);
        QCOMPARE(contents(path), yaml);
        QVERIFY(QFile::remove(path));
        QVERIFY_EXCEPTION_THROWN(SettingsStore::profilePath(dir.path()), std::runtime_error);
        QVERIFY(!QFile::exists(path));
        QCOMPARE(contents(ini + ".migrated"), original);
    }

    void invalidYamlIsNeverReplaced_data() {
        QTest::addColumn<QByteArray>("yaml");
        const QByteArray header = "format: WHAT settings\nversion: 1\nsettings:\n";
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("truncated") << header + "  selected: [";
        QTest::newRow("duplicate") << header + "  port: 48765\n  port: 48766\n";
        QTest::newRow("wrong-version") << QByteArray("format: WHAT settings\nversion: 2\nsettings: {}\n");
        QTest::newRow("invalid-utf8") << header + QByteArray("  name: \xff\n");
        QTest::newRow("binary") << header + "  token: !!binary 'not-base64!'\n";
        QTest::newRow("cycle") << header + "  loop: &loop [*loop]\n";
        QTest::newRow("tag") << header + "  object: !!python/object:evil {}\n";
        QTest::newRow("multiple-documents") << header + "  port: 48765\n---\nsettings: {}\n";
    }

    void invalidYamlIsNeverReplaced() {
        QFETCH(QByteArray, yaml);
        QTemporaryDir dir;
        const auto path = dir.filePath("settings.yaml");
        write(path, yaml);
        write(dir.filePath("settings.ini"), "[General]\ndevice_id=do-not-use\n");
        QVERIFY_EXCEPTION_THROWN(SettingsStore::profilePath(dir.path()), std::runtime_error);
        {
            QSettings settings(path, SettingsStore::format());
            settings.setValue("device_id", "do-not-write");
            settings.sync();
            QVERIFY(settings.status() != QSettings::NoError);
        }
        QCOMPARE(contents(path), yaml);
        QVERIFY(QFile::exists(dir.filePath("settings.ini")));
    }

    void workerAndUiShareOneProfile() {
        QTemporaryDir dir;
        const auto path = SettingsStore::profilePath(dir.path());
        QSettings ui(path, SettingsStore::format());
        QSettings updater(path, SettingsStore::format());
        ui.setValue("ui/window_mode", "maximised");
        updater.setValue("updates/beta", true);
        QSettings::Status result = QSettings::AccessError;
        std::thread worker([&] {
            QSettings storage(path, SettingsStore::format());
            storage.setValue("catalog", QByteArray("[]"));
            storage.sync();
            result = storage.status();
        });
        worker.join();
        QCOMPARE(result, QSettings::NoError);
        QCOMPARE(ui.value("catalog").toByteArray(), QByteArray("[]"));
        QVERIFY(updater.value("updates/beta").toBool());
        const auto copy = dir.filePath("copy.yaml");
        QVERIFY(QFile::copy(path, copy));
        QSettings persisted(copy, SettingsStore::format());
        QCOMPARE(persisted.value("ui/window_mode").toString(), QString("maximised"));
        QVERIFY(persisted.value("updates/beta").toBool());
        QCOMPARE(persisted.value("catalog").toByteArray(), QByteArray("[]"));
    }
};

QTEST_GUILESS_MAIN(SettingsTests)
#include "settings_tests.moc"
