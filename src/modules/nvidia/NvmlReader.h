#pragma once
#include <QJsonObject>

// Minimal NVML ABI declarations. No NVIDIA runtime is redistributed.
// https://docs.nvidia.com/deploy/nvml-api/
namespace Nvidia {
using Device = void *;

struct Memory {
    unsigned long long total, free, used;
};

struct Utilization {
    unsigned int gpu, memory;
};

static_assert(sizeof(Memory) == 24 && sizeof(Utilization) == 8);

struct Api {
    int (*count)(unsigned int *) = nullptr;
    int (*handle)(unsigned int, Device *) = nullptr;
    int (*name)(Device, char *, unsigned int) = nullptr;
    int (*uuid)(Device, char *, unsigned int) = nullptr;
    int (*driver)(char *, unsigned int) = nullptr;
    int (*temperature)(Device, unsigned int, unsigned int *) = nullptr;
    int (*utilization)(Device, Utilization *) = nullptr;
    int (*memory)(Device, Memory *) = nullptr;
    int (*fan)(Device, unsigned int *) = nullptr;
    int (*power)(Device, unsigned int *) = nullptr;
    int (*powerLimit)(Device, unsigned int *) = nullptr;
    int (*clock)(Device, unsigned int, unsigned int *) = nullptr;
    int (*encoder)(Device, unsigned int *, unsigned int *) = nullptr;
    int (*decoder)(Device, unsigned int *, unsigned int *) = nullptr;
    const char *(*error)(int) = nullptr;
};

QJsonObject collect(const Api &api);
QJsonObject read();
} // namespace Nvidia
