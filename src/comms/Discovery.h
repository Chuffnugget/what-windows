#pragma once
#include <QObject>
#include <memory>
struct DiscoveryState;

class Discovery : public QObject {
    Q_OBJECT
public:
    explicit Discovery(QObject *parent = nullptr);
    ~Discovery() override;
    void start(const QString &id, quint16 port);
    void stop();
signals:
    void status(const QString &message);

private:
    std::shared_ptr<DiscoveryState> state;
};
