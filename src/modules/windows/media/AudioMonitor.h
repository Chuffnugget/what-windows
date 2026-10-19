#pragma once
#include <QThread>
#include <QObject>
#include <windows.h>
#include <memory>

// Callbacks post scalar data only. COM ownership and teardown stay on one QThread.
class AudioMonitor {
public:
    static constexpr UINT VolumeMessage = WM_APP + 144;
    static constexpr UINT DeviceMessage = WM_APP + 145;
    AudioMonitor();
    ~AudioMonitor();
    void start(HWND window);
    void refresh();
    void stop();

private:
    QThread *thread = new QThread;
    QObject *context;
    struct State;
    std::shared_ptr<State *> state = std::make_shared<State *>(nullptr);
};
