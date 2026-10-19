# Third-party components in the Windows binary

WHAT source is LGPL-3.0-or-later. This package dynamically links Qt and
OpenSSL; users may replace these libraries and rebuild/relink WHAT. All our
C++/QML source and build scripts are provided in the matching Source.zip release asset.
The matching Qt source archives are published alongside each release; see REBUILDING.md. There is no
restriction on reverse engineering for debugging modified library versions.

## Qt 6.11.2

Copyright The Qt Company and other Qt contributors. Uses Qt Base (Core, GUI,
Widgets, Network, OpenGL, Concurrent), Qt Declarative (QML, Quick, Controls, Layouts, Shapes),
Qt WebSockets and Qt SVG, under their LGPLv3 option. No Qt MQTT, HTTP Server,
Virtual Keyboard or Lottie module is required or shipped.

Exact, unmodified corresponding source is freely available from Qt:
https://download.qt.io/archive/qt/6.11/6.11.2/single/qt-everywhere-src-6.11.2.tar.xz
Individual module archives are also available at:
https://download.qt.io/archive/qt/6.11/6.11.2/submodules/

The directories beside this file preserve Qt's licence texts and embedded
third-party copyright/licence notices. `qt_attribution.json` files identify
third-party packages and their terms. The supplied upstream module SBOMs are
broader than this application's deployed subset and include source/build
components not used at runtime. Example programs, test fixtures and Qt licence-
checking tools are omitted from this notices directory; the matching Qt source
archives contain them. Qt itself is dynamically linked without local source
modifications. Use a Qt kit matching the release architecture to build replacement DLLs.

## OpenSSL

Copyright The OpenSSL Project Authors. Apache License 2.0 (included).
The generated DEPENDENCIES.json records the actual deployed library versions,
hashes and build-tool provenance. No FIPS validation is claimed. CI uses Qt's
OpenSSL toolkit for Windows x64; local builds require MSVC import libraries.
Upstream source: https://github.com/openssl/openssl
Qt toolkit distribution: https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_opensslv3_x64/

## Microsoft Visual C++ runtime

The app and Qt use the Microsoft Visual C++ runtime. Release CRT DLLs from the
builder's Visual Studio redistributable directory are deployed app-local under
Microsoft's redistribution terms, including in portable and MSIX distributions.
The Windows helper statically links its runtime. No MinGW runtime is shipped.

## Microsoft Edge WebView2 SDK 1.0.4258.31

Copyright Microsoft Corporation. WHAT statically links the unmodified WebView2
loader and uses its headers to host the optional Home Assistant Web module.
CMake fetches the official NuGet archive with a pinned SHA-256. The package's
licence and notices are included as `WebView2-SDK-LICENSE.txt` and
`WebView2-SDK-NOTICE.txt`. The separately installed WebView2 Runtime is supplied
and updated by Microsoft, and is not included in WHAT's downloads.
Upstream package: https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4258.31

HWiNFO is not bundled and is not licensed by this project. It is a separate
product of its respective authors. No HWiNFO SDK header is included.

## Windows companion

WindowsBridge uses Microsoft C++/WinRT headers (MIT; CppWinRT-MIT.txt) and the Microsoft Visual C++ runtime statically linked under Visual Studio redistribution terms. Copyright Microsoft Corporation. Windows system APIs are provided by Windows itself. The bridge source is LGPL-3.0-or-later and can be rebuilt with the documented Visual Studio/Windows SDK toolchain. Source for C++/WinRT: https://github.com/microsoft/cppwinrt

## yaml-cpp 0.9.0

YAML settings reader and writer, statically linked under the MIT licence. See
`yaml-cpp-MIT.txt`. CMake fetches the unmodified source archive with a pinned SHA-256:
https://github.com/jbeder/yaml-cpp/tree/yaml-cpp-0.9.0

## Eclipse Paho MQTT C 1.3.15

Statically linked asynchronous TLS MQTT client, used under the Eclipse Distribution License 1.0 option. The unmodified source is fetched by CMake from https://github.com/eclipse-paho/paho.mqtt.c/tree/v1.3.15 with its archive SHA-256 pinned. See `paho-edl-v10.txt`, `paho-epl-v20.txt` and `paho-LICENSE.txt`. Paho is separate from the GPL/commercial Qt MQTT module, which is not used.
