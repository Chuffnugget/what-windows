#include "modules/windows/media/AudioMonitor.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <QTimer>
#include <atomic>
#include <cmath>

namespace {
class Events : public IAudioEndpointVolumeCallback, public IMMNotificationClient {
    std::atomic<ULONG> refs{1};
    HWND window;

public:
    explicit Events(HWND target) : window(target) {}

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --refs;
        if (!n)
            delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IAudioEndpointVolumeCallback))
            *out = static_cast<IAudioEndpointVolumeCallback *>(this);
        else if (id == __uuidof(IMMNotificationClient))
            *out = static_cast<IMMNotificationClient *>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
        if (!data)
            return E_POINTER;
        PostMessageW(window, AudioMonitor::VolumeMessage, WPARAM(std::lround(data->fMasterVolume * 1000)),
                     LPARAM(data->bMuted != FALSE));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eMultimedia)
            PostMessageW(window, AudioMonitor::DeviceMessage, 0, 0);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        PostMessageW(window, AudioMonitor::DeviceMessage, 0, 0);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        PostMessageW(window, AudioMonitor::DeviceMessage, 0, 0);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }
};
} // namespace

struct AudioMonitor::State {
    IMMDeviceEnumerator *enumerator = nullptr;
    IAudioEndpointVolume *volume = nullptr;
    Events *events = nullptr;
    bool com = false;

    void bind() {
        if (volume) {
            volume->UnregisterControlChangeNotify(events);
            volume->Release();
            volume = nullptr;
        }
        IMMDevice *device = nullptr;
        if (enumerator && SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) {
            if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void **>(&volume))))
                volume->RegisterControlChangeNotify(events);
            device->Release();
        }
    }

    ~State() {
        if (volume) {
            volume->UnregisterControlChangeNotify(events);
            volume->Release();
        }
        if (enumerator) {
            enumerator->UnregisterEndpointNotificationCallback(events);
            enumerator->Release();
        }
        if (events)
            events->Release();
        if (com)
            CoUninitialize();
    }
};

AudioMonitor::AudioMonitor() : context(new QObject) {
    thread->setObjectName("Windows audio events");
    context->moveToThread(thread);
    QObject::connect(thread, &QThread::finished, context, &QObject::deleteLater);
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

AudioMonitor::~AudioMonitor() {
    QMetaObject::invokeMethod(
        context,
        [slot = state, t = thread] {
            delete *slot;
            *slot = nullptr;
            t->quit();
        },
        Qt::QueuedConnection);
    thread->wait(250);
}

void AudioMonitor::start(HWND window) {
    QMetaObject::invokeMethod(context, [slot = state, window] {
        auto &state = *slot;
        if (state)
            return;
        state = new State;
        state->com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (!state->com)
            return;
        state->events = new Events(window);
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void **>(&state->enumerator)))) {
            state->enumerator->RegisterEndpointNotificationCallback(state->events);
            state->bind();
        }
    });
}

void AudioMonitor::refresh() {
    QMetaObject::invokeMethod(context, [slot = state] {
        if (*slot)
            (*slot)->bind();
    });
}

void AudioMonitor::stop() {
    QMetaObject::invokeMethod(context, [slot = state] {
        delete *slot;
        *slot = nullptr;
    });
}
