# NVIDIA GPU monitoring

Enable **Modules > NVIDIA** in WHAT. No HWiNFO, CUDA toolkit, NVIDIA SDK download or MQTT broker
is needed. The installed NVIDIA graphics driver supplies NVML.

NVML is the native API underneath nvidia-smi. WHAT calls it directly on a
separate QThread worker, without spawning nvidia-smi or blocking the GUI.
Only absolute driver-library paths are used (System32 and the standard NVIDIA
NVSMI installation directory). The NVIDIA library is not redistributed.

Each detected GPU has its own browser category and UUID-based sensor identities.
Available queries: core temperature, GPU utilisation, memory-controller activity,
VRAM total/used/free, fan percentage, power draw/limit, graphics and memory clocks,
encoder/decoder utilisation, model, UUID and driver version. Readings vary by
GPU and driver; hot-spot/VRAM junction temperatures are not promised by this API.
Memory-controller activity is not the percentage of VRAM capacity allocated.
Fan percentage is not RPM. Power is converted from milliwatts to watts.

The module defaults off and polls every 2000 ms when enabled. The slider covers
1000 to 10000 ms; numeric input supports 500 to 60000 ms. Settings save automatically.
Polling can wake a laptop GPU and affect battery use; disable the module when
this is unwanted. Disabling hides its browser entries immediately and removes
its readings from the shared stream. Selection/HA entity identities are retained.
NVIDIA and HWiNFO readings can coexist; they are not automatically deduplicated.

Unsupported queries are omitted. A previously discovered reading becomes
unavailable if a later query fails; it never becomes a fabricated zero. The
inventory is enumerated on every poll, allowing recovery after driver/device
changes. Diagnostics includes the driver-library path, detected GPU tree,
unsupported queries and per-query errors. It includes GPU UUIDs; review before
sharing. No fan, clock, power-limit or other GPU settings are modified.

The optional `nvidia_probe` CMake target prints a read-only NVML report. Tests
cover multiple GPUs, unique IDs, units, missing/unsupported APIs, failed queries,
module lifecycle and NVIDIA readings with HWiNFO disabled. A live probe on an
NVIDIA GeForce RTX 4090 returned 16 readings. Other hardware has not been tested.

API reference: https://docs.nvidia.com/deploy/nvml-api/nvml-api-reference.html
