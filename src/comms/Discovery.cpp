#include "comms/Discovery.h"
#include <QCoreApplication>
#include <QHostInfo>
#include <QPointer>
#include <winsock2.h>
#include <windows.h>
#include <windns.h>

struct DiscoveryState {
    QPointer<Discovery> owner;
    QString id, registeredId;
    quint16 port = 0, registeredPort = 0;
    bool enabled = false, busy = false, registered = false;
    DNS_SERVICE_INSTANCE *instance = nullptr;

    ~DiscoveryState() {
        if (instance)
            DnsServiceFreeInstance(instance);
    }
};

namespace {
struct Operation {
    std::shared_ptr<DiscoveryState> state;
    DNS_SERVICE_REGISTER_REQUEST request{};
    bool registering;
};

void advance(const std::shared_ptr<DiscoveryState> &s);

void finish(Operation *operation, DWORD result, DNS_SERVICE_INSTANCE *returned) {
    const auto s = operation->state;
    const bool registering = operation->registering;
    if (returned && returned != s->instance)
        DnsServiceFreeInstance(returned);
    delete operation;
    s->busy = false;
    if (registering && result == ERROR_SUCCESS)
        s->registered = true;
    else {
        s->registered = false;
        if (s->instance)
            DnsServiceFreeInstance(s->instance);
        s->instance = nullptr;
    }
    if (s->owner)
        emit s->owner->status(
            result != ERROR_SUCCESS ? QString("Discovery error %1; use the IP and port").arg(result)
            : registering           ? QString("Discovery: advertised on port %1").arg(s->registeredPort)
                                    : "Discovery stopped");
    if (result == ERROR_SUCCESS)
        advance(s); // Never retry failures in an unbounded loop.
}

void WINAPI complete(DWORD result, void *context, DNS_SERVICE_INSTANCE *instance) {
    auto *operation = static_cast<Operation *>(context);
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [operation, result, instance] { finish(operation, result, instance); },
        Qt::QueuedConnection);
}

void advance(const std::shared_ptr<DiscoveryState> &s) {
    if (s->busy)
        return;
    const bool current =
        s->registered && s->enabled && s->registeredPort == s->port && s->registeredId == s->id;
    if (current || (!s->registered && !s->enabled))
        return;
    const bool registering = !s->registered;
    if (registering) {
        const auto service = ("WHAT-" + s->id.left(8) + "._what._tcp.local").toStdWString();
        const auto host = (QHostInfo::localHostName().section('.', 0, 0) + ".local").toStdWString();
        const auto id = s->id.toStdWString();
        // The integration shows this as the default name in its discovery card.
        const auto name = QHostInfo::localHostName().section('.', 0, 0).left(63).toStdWString();
        PCWSTR keys[] = {L"id", L"version", L"tls", L"name"};
        PCWSTR values[] = {id.c_str(), L"2", L"1", name.c_str()};
        s->instance = DnsServiceConstructInstance(service.c_str(), host.c_str(), nullptr, nullptr, s->port, 0,
                                                  0, 4, keys, values);
        if (!s->instance) {
            if (s->owner)
                emit s->owner->status("Discovery unavailable; use the IP and port");
            return;
        }
        s->registeredPort = s->port;
        s->registeredId = s->id;
    }
    auto *operation = new Operation{s, {}, registering};
    operation->request.Version = 1;
    operation->request.pServiceInstance = s->instance;
    operation->request.pRegisterCompletionCallback = complete;
    operation->request.pQueryContext = operation;
    s->busy = true;
    const auto result = registering ? DnsServiceRegister(&operation->request, nullptr)
                                    : DnsServiceDeRegister(&operation->request, nullptr);
    if (result != DNS_REQUEST_PENDING)
        finish(operation, result, nullptr);
}
} // namespace

Discovery::Discovery(QObject *parent) : QObject(parent), state(std::make_shared<DiscoveryState>()) {
    state->owner = this;
}

Discovery::~Discovery() {
    state->owner = nullptr;
    stop();
}

void Discovery::start(const QString &id, quint16 port) {
    state->id = id;
    state->port = port;
    state->enabled = true;
    advance(state);
}

void Discovery::stop() {
    state->enabled = false;
    advance(state);
}
