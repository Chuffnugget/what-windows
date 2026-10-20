cmake_minimum_required(VERSION 3.21)
project(
    WHAT
    VERSION 0.0.1
    LANGUAGES C CXX RC
)
# Leave empty for stable releases; use beta.1, beta.2, etc. for prereleases.
#set(WHAT_PRERELEASE "")
set(WHAT_PRERELEASE "beta.5")
if(WHAT_PRERELEASE AND NOT WHAT_PRERELEASE MATCHES "^beta\\.[1-9][0-9]*$")
    message(FATAL_ERROR "WHAT_PRERELEASE must be empty or beta.N")
endif()
set(WHAT_RELEASE_VERSION "${PROJECT_VERSION}")
if(WHAT_PRERELEASE)
    string(APPEND WHAT_RELEASE_VERSION "-${WHAT_PRERELEASE}")
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/what-version.txt" CONTENT "${WHAT_RELEASE_VERSION}\n")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Source files and protocol/UI strings use UTF-8, independent of the PC locale.
add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>")
find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Quick QuickControls2 Widgets Network WebSockets Concurrent ShaderTools)
# MSVC reports the target architecture, even when CMake runs on an x64 host.
if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
    set(WHAT_ARCHITECTURE arm64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ARM|aarch64")
    set(WHAT_ARCHITECTURE x64)
else()
    message(FATAL_ERROR "WHAT supports native MSVC x64 and ARM64 targets only.")
endif()
message(STATUS "WHAT target architecture: ${WHAT_ARCHITECTURE}")
if(WHAT_ARCHITECTURE STREQUAL "arm64" AND NOT OPENSSL_ROOT_DIR)
    set(OPENSSL_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/dependencies/openssl-arm64")
endif()
find_package(OpenSSL 3 REQUIRED)
# MinGW's include directory also contains incompatible Windows and CRT headers.
if(MSVC AND EXISTS "${OPENSSL_INCLUDE_DIR}/_mingw.h")
    message(FATAL_ERROR
        "MSVC cannot use MinGW OpenSSL headers from ${OPENSSL_INCLUDE_DIR}. "
        "Set OPENSSL_ROOT_DIR to a matching ${WHAT_ARCHITECTURE} MSVC OpenSSL installation "
        "(Qt: Tools/OpenSSLv3/Win_x64), clear the cached OPENSSL_*, LIB_EAY_* "
        "and SSL_EAY_* entries, then reconfigure."
    )
endif()
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
set(PAHO_BUILD_SHARED
    OFF
    CACHE BOOL "" FORCE
)
set(PAHO_BUILD_STATIC
    ON
    CACHE BOOL "" FORCE
)
set(PAHO_WITH_SSL
    ON
    CACHE BOOL "" FORCE
)
set(PAHO_BUILD_SAMPLES
    OFF
    CACHE BOOL "" FORCE
)
set(PAHO_BUILD_DOCUMENTATION
    OFF
    CACHE BOOL "" FORCE
)
set(PAHO_ENABLE_TESTING
    OFF
    CACHE BOOL "" FORCE
)
FetchContent_Declare(
    paho
    URL https://github.com/eclipse-paho/paho.mqtt.c/archive/refs/tags/v1.3.15.zip
    URL_HASH SHA256=d64ea8d1c4ea10c76a7553fedb7de60c60c05a655c4dae1580bb1ff902bd85b9
)
FetchContent_MakeAvailable(paho)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    yaml_cpp
    URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.9.0.zip
    URL_HASH SHA256=1c22709eb1fcde200c87ef4e878ce2c9477cc05eae84ebf1f72ec5b356468fee
)
FetchContent_MakeAvailable(yaml_cpp)
qt_standard_project_setup()
# The browser runtime is supplied and updated by Microsoft. Only its small
# loader is linked into WHAT; the SDK archive is pinned for reproducible builds.
FetchContent_Declare(webview2
    URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4258.31/microsoft.web.webview2.1.0.4258.31.nupkg
    URL_HASH SHA256=56f7f4b8bf9aee4b8efefbbdd4f67d5f74ebd1b100ed0806da71bf76af481aa9
    DOWNLOAD_NAME webview2.zip
)
FetchContent_MakeAvailable(webview2)
add_library(what_haweb STATIC src/modules/haweb/HomeAssistantWeb.cpp src/modules/haweb/HomeAssistantWeb.h
    src/modules/haweb/HomeAssistantWebBrowser.cpp
    src/modules/haweb/HomeAssistantWebPermissions.cpp
    src/modules/haweb/HomeAssistantWebState.h src/modules/haweb/WebOptions.h
    src/modules/haweb/BrowserScript.h src/modules/haweb/SidebarScript.h src/modules/haweb/DesktopPlacement.h)
target_include_directories(what_haweb PUBLIC src PRIVATE "${webview2_SOURCE_DIR}/build/native/include")
target_compile_definitions(what_haweb PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(what_haweb PUBLIC Qt6::Quick PRIVATE
    "${webview2_SOURCE_DIR}/build/native/${WHAT_ARCHITECTURE}/WebView2LoaderStatic.lib" ole32 user32 shlwapi version dwmapi)
add_library(what_settings STATIC src/app/SettingsStore.cpp src/app/SettingsStore.h)
target_compile_features(what_settings PRIVATE cxx_std_20)
target_include_directories(what_settings PUBLIC src)
target_link_libraries(what_settings PUBLIC Qt6::Core PRIVATE yaml-cpp::yaml-cpp)
add_library(
    beacn_backend STATIC
    src/modules/beacn/BeacnBackend.cpp src/modules/beacn/BeacnBackend.h
    src/modules/beacn/InstalledFiles.cpp src/modules/beacn/BeacnModule.cpp
    src/modules/beacn/BeacnModule.h src/modules/beacn/NativeLayout.h
)
target_compile_features(beacn_backend PRIVATE cxx_std_20)
target_compile_definitions(beacn_backend PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(
    beacn_backend PUBLIC Qt6::Core Qt6::Concurrent ole32 oleaut32 uuid advapi32 setupapi version
)
if(WHAT_ARCHITECTURE STREQUAL "x64")
    add_library(beacn-volume-hook SHARED src/modules/beacn/VolumeHook.cpp)
    # The injected hook must not depend on the CRT bundled with BEACN.
    set_target_properties(beacn-volume-hook PROPERTIES PREFIX "" AUTOMOC OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_features(beacn-volume-hook PRIVATE cxx_std_20)
    target_link_libraries(beacn-volume-hook PRIVATE user32)
    if(MINGW)
        target_link_options(beacn-volume-hook PRIVATE -static)
    endif()
    install(TARGETS beacn-volume-hook RUNTIME DESTINATION .)
endif()
qt_add_executable(
    WHAT
    WIN32
    src/app/app-icon.rc
    src/main.cpp
    src/updater/UpdateManager.cpp
    src/updater/UpdateDownload.cpp
    src/updater/UpdateManager.h
    src/modules/windows/specs/Specs.cpp
    src/modules/windows/specs/SpecsHardware.cpp
    src/comms/MqttBridge.cpp
    src/comms/MqttBridge.h
    src/comms/NetworkService.cpp
    src/comms/NetworkInstallations.cpp
    src/comms/WirePeer.h
    src/comms/NetworkService.h
    src/app/ControllerMqtt.cpp
    src/app/ControllerInstallations.cpp
    src/modules/wallpaper/WallpaperEngine.cpp
    src/modules/wallpaper/ControllerWallpaper.cpp
    src/modules/beacn/ControllerBeacn.cpp
    src/app/SensorTrayManager.cpp
    src/app/SensorTrayManager.h
    src/app/TaskbarIconManager.cpp
    src/app/TaskbarIconManager.h
    src/app/Controller.cpp
    src/app/DiagnosticReport.cpp
    src/app/AppRuntime.cpp
    src/app/ControllerApp.cpp
    src/app/ColourPalette.cpp
    src/app/ColourPalette.h
    src/app/SoftwareVersions.cpp
    src/app/ConfigurationFile.cpp
    src/app/ControllerConfiguration.cpp
    src/app/Controller.h
    src/app/LocalDiagnostics.h
    src/modules/windows/lighting/DynamicLighting.cpp
    src/modules/windows/lighting/ControllerLighting.cpp
    src/modules/coretemp/CoreTempReader.cpp
    src/modules/coretemp/ControllerCoreTemp.cpp
    src/modules/hwinfo/HwinfoReader.cpp
    src/modules/hwinfo/HwinfoReader.h
    src/comms/TlsIdentity.cpp
    src/comms/TlsIdentity.h
    src/comms/Discovery.cpp
    src/comms/Discovery.h
    src/modules/windows/WindowsCollector.cpp
    src/modules/windows/WindowsCollector.h
    src/app/ControllerWindows.cpp
    src/app/ControllerModuleIssues.cpp
    src/modules/windows/displays/Displays.cpp
    src/modules/windows/displays/ControllerDisplays.cpp
    src/modules/windows/commands/ControllerCommands.cpp
    src/modules/windows/notifications/ControllerNotifications.cpp
    src/modules/windows/commands/WindowsCommands.cpp
    src/modules/windows/commands/ShellCommand.cpp
    src/modules/custom/CustomSources.cpp
    src/modules/custom/CustomModule.cpp
    src/modules/custom/CustomModule.h
    src/modules/custom/ControllerCustom.cpp
    src/modules/icue/IcueAutomation.cpp
    src/modules/icue/LightingLink.cpp
    src/modules/icue/IcueSdk.cpp
    src/modules/icue/ControllerIcue.cpp
    src/modules/nvidia/NvmlReader.cpp
    src/modules/nvidia/ControllerNvidia.cpp
    src/modules/windows/media/AudioMonitor.cpp
)
target_sources(WHAT PRIVATE src/comms/WirePeer.h)
# The shared attached tooltip is loaded by style name from Qt's QML import path.
set_source_files_properties(qml/styles/WHATStyle/qmldir
    PROPERTIES QT_RESOURCE_ALIAS "qt/qml/WHATStyle/qmldir")
set_source_files_properties(qml/styles/WHATStyle/ToolTip.qml
    PROPERTIES QT_RESOURCE_ALIAS "qt/qml/WHATStyle/ToolTip.qml")
set(WHAT_QML_FILES
    qml/Main.qml
    qml/controls/GlassDialog.qml
    qml/dialogs/DisplayRecoveryPopup.qml
    qml/dialogs/QuitDialog.qml
    qml/dialogs/EntityCleanupDialog.qml
    qml/dialogs/UpdateNoticeDialog.qml
    qml/dialogs/ClearWebDataDialog.qml
    qml/dialogs/ConfigurationImportDialog.qml
    qml/dialogs/AnimationTimingsDialog.qml
    qml/dialogs/UpdateConfirmationDialog.qml
    qml/dialogs/ReleaseBrowserDialog.qml
    qml/dialogs/CertificateDialog.qml
    qml/dialogs/WallpaperControlsDialog.qml
    qml/dialogs/HwinfoSetupDialog.qml
    qml/dialogs/DiagnosticReportDialog.qml
    qml/dialogs/PairingTokenDialog.qml
    qml/styles/WHATStyle/qmldir
    qml/styles/WHATStyle/ToolTip.qml
    qml/components/AnimatedLogo.qml
    qml/components/DiagnosticTextView.qml
    qml/components/DisplayControls.qml
    qml/components/FluidWindowTransition.qml
    qml/components/GlassSurface.qml
    qml/components/GlassBackdrop.qml
    qml/components/NavigationBar.qml
    qml/components/PageStack.qml
    qml/components/PageHeading.qml
    qml/components/PopupTransition.qml
    qml/components/SavedColours.qml
    qml/components/ServiceControls.qml
    qml/components/ConnectionHeartbeats.qml
    qml/components/ConnectionHeartbeat.qml
    qml/components/HeartbeatTrace.js
    qml/controls/AddListEntry.qml
    qml/controls/ActionButton.qml
    qml/controls/ChoiceBox.qml
    qml/controls/ValueSlider.qml
    qml/controls/ContextMenu.qml
    qml/controls/ContextMenuItem.qml
    qml/controls/PromptButtons.qml
    qml/controls/Card.qml
    qml/controls/ChevronButton.qml
    qml/controls/DragScrollView.qml
    qml/controls/ExpansionScroll.qml
    qml/controls/ExpansionBlur.qml
    qml/controls/Accordion.js
    qml/controls/FastWheel.qml
    qml/controls/HintLabel.qml
    qml/controls/InputField.qml
    qml/controls/MillisInput.qml
    qml/controls/ModuleSection.qml
    qml/controls/NavigationButton.qml
    qml/controls/NumberInput.qml
    qml/controls/OptionCheckBox.qml
    qml/controls/PermissionBadge.qml
    qml/controls/PermissionBadges.qml
    qml/controls/QuietButton.qml
    qml/controls/SettingsSection.qml
    qml/controls/OpenCloseLayer.qml
    qml/components/InstallationList.qml
    qml/components/InstallationEditor.qml
    qml/components/InstallationPairing.qml
    qml/controls/PopupMotion.qml
    qml/controls/Theme.qml
    qml/controls/TimingSlider.qml
    qml/controls/ToggleSwitch.qml
    qml/controls/WarningBanner.qml
    qml/controls/WideScrollBar.qml
    qml/controls/qmldir
    qml/modules/BeacnSettings.qml
    qml/modules/ControlOption.qml
    qml/modules/CoreTempSettings.qml
    qml/modules/CustomSettings.qml
    qml/controls/PermissionOption.qml
    qml/custom/CommandEditor.qml
    qml/custom/CommandPermissions.qml
    qml/custom/CommandRow.qml
    qml/custom/CommandSection.qml
    qml/custom/DefinitionDialog.qml
    qml/custom/EditorResult.qml
    qml/custom/SensorEditor.qml
    qml/custom/SensorLibrary.qml
    qml/custom/ShellEditor.qml
    qml/pages/CommandsPage.qml
    qml/modules/HwinfoSettings.qml
    qml/modules/IcueSettings.qml
    qml/modules/ModuleCard.qml
    qml/modules/ModuleReorderArea.qml
    qml/modules/ModuleReorderGrip.qml
    qml/modules/ModuleStatus.js
    qml/modules/NvidiaSettings.qml
    qml/modules/PollingInput.qml
    qml/modules/SourceOption.qml
    qml/modules/WallpaperSettings.qml
    qml/modules/WindowsSettings.qml
    qml/pages/AboutPage.qml
    qml/pages/DiagnosticsPage.qml
    qml/pages/ModulesPage.qml
    qml/pages/HomeAssistantWebPage.qml
    qml/components/HomeAssistantWebHost.qml
    qml/modules/HomeAssistantWebSettings.qml
    qml/modules/HomeAssistantWebOptions.qml
    qml/pages/OverviewPage.qml
    qml/pages/SensorsPage.qml
    qml/sensors/BrowserBeacn.qml
    qml/sensors/BrowserButton.qml
    qml/sensors/BrowserChoice.qml
    qml/sensors/BrowserColour.qml
    qml/sensors/BrowserDeviceControls.qml
    qml/sensors/BrowserMeter.qml
    qml/sensors/BrowserWallpaper.qml
    qml/sensors/HaEntityDetails.qml
    qml/pages/SettingsPage.qml
    qml/pages/SpecsPage.qml
    qml/pages/TaskbarPage.qml
)
# Compile Direct3D bytecode during the build when the SDK tool is available.
# Other toolchains can still package HLSL for Qt's normal runtime compilation.
find_program(WHAT_FXC_EXECUTABLE NAMES fxc NO_CACHE)
set(WHAT_SHADER_OPTIONS)
if(WIN32 AND WHAT_FXC_EXECUTABLE)
    list(APPEND WHAT_SHADER_OPTIONS PRECOMPILE)
endif()
qt_add_shaders(WHAT openclose_shader
    ${WHAT_SHADER_OPTIONS}
    PREFIX "/shaders"
    BASE "qml/shaders"
    FILES qml/shaders/openclose.frag
)
qt_add_resources(
    WHAT
    ui
    PREFIX
    "/"
    FILES
    ${WHAT_QML_FILES}
    src/modules/windows/specs/SpecsInventory.ps1
    assets/logo-roof.svg
    assets/app-icon.png
    assets/app-icon.ico
    assets/update-available.svg
    assets/brands/buy-me-a-coffee.svg
    assets/brands/paypal.svg
    assets/brands/corsair.svg
    assets/brands/coretemp.svg
    assets/brands/hwinfo.png
    assets/brands/nvidia.svg
    assets/brands/windows.svg
    assets/brands/wallpaper.png
    assets/brands/beacn.svg
    assets/brands/github-sponsors.svg
    assets/Logo150.png
    assets/chevron.svg
    scripts/Apply-Update.ps1
)
# Also prepare dependencies for direct Qt Creator / Debug builds. An always-run dependency repairs
# missing files even when the executable needs no relink.
if(WHAT_ARCHITECTURE STREQUAL "x64")
    add_custom_target(
        prepare_icue_runtime
        COMMAND
            powershell.exe -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Prepare-iCueSdk.ps1" -Destination
            "$<TARGET_FILE_DIR:WHAT>/helpers"
        COMMENT "Preparing iCUE SDK and its app-local Microsoft runtime"
        VERBATIM
    )
    add_dependencies(WHAT prepare_icue_runtime)
endif()
target_include_directories(WHAT PRIVATE assets src)
set_property(
    SOURCE src/app/app-icon.rc
    APPEND
    PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/app-icon.ico"
)
if(TARGET beacn-volume-hook)
    add_dependencies(WHAT beacn-volume-hook)
endif()
target_link_libraries(
    WHAT
    PRIVATE beacn_backend what_settings
            Qt6::Quick
            Qt6::QuickControls2
            Qt6::Widgets
            Qt6::Network
            Qt6::WebSockets
            Qt6::Concurrent
            OpenSSL::Crypto
            paho-mqtt3as-static
            dwmapi
            crypt32
            version
            dnsapi
            ws2_32
            psapi
            iphlpapi
            wlanapi
            wtsapi32
            ole32
            uuid
            wevtapi
            user32
            dxgi
            tbs
            cfgmgr32
            slc
            shcore
            pdh
            powrprof
            shell32
)
target_compile_definitions(WHAT PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_definitions(WHAT PRIVATE WHAT_VERSION="${WHAT_RELEASE_VERSION}")
qt_add_executable(icon_builder EXCLUDE_FROM_ALL tools/make-icons.cpp)
add_executable(icue_runtime_probe EXCLUDE_FROM_ALL tools/probe-icue-runtime.cpp)
# The probe must not preload the machine's dynamic CRT before testing the SDK.
set_target_properties(icue_runtime_probe PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_link_options(icue_runtime_probe PRIVATE $<$<CXX_COMPILER_ID:GNU>:-municode>)
target_link_libraries(icon_builder PRIVATE Qt6::Gui)
include(CTest)
if(BUILD_TESTING)
    add_library(icue_sdk_fixture SHARED tests/icue_sdk_fixture.cpp)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(
        controller_tests
        src/updater/UpdateManager.cpp
        src/updater/UpdateDownload.cpp
        tests/controller_tests.cpp
        src/modules/windows/specs/Specs.cpp
        src/modules/windows/specs/SpecsHardware.cpp
        src/comms/MqttBridge.cpp
        src/comms/MqttBridge.h
        src/comms/NetworkService.cpp
        src/comms/NetworkInstallations.cpp
        src/comms/WirePeer.h
        src/comms/NetworkService.h
        src/app/ControllerMqtt.cpp
        src/app/ControllerInstallations.cpp
        src/modules/wallpaper/WallpaperEngine.cpp
        src/modules/wallpaper/ControllerWallpaper.cpp
        src/modules/beacn/ControllerBeacn.cpp
        src/app/SensorTrayManager.cpp
        src/app/SensorTrayManager.h
        src/app/TaskbarIconManager.cpp
        src/app/TaskbarIconManager.h
        src/app/Controller.cpp
        src/app/DiagnosticReport.cpp
        src/app/AppRuntime.cpp
        src/app/ControllerApp.cpp
        src/app/ColourPalette.cpp
        src/app/ColourPalette.h
        src/app/SoftwareVersions.cpp
        src/app/ConfigurationFile.cpp
        src/app/ControllerConfiguration.cpp
        src/app/ControllerWindows.cpp
        src/app/ControllerModuleIssues.cpp
        src/modules/windows/displays/Displays.cpp
        src/modules/windows/displays/ControllerDisplays.cpp
        src/modules/windows/commands/ControllerCommands.cpp
        src/modules/windows/notifications/ControllerNotifications.cpp
        src/modules/windows/commands/WindowsCommands.cpp
        src/modules/windows/commands/ShellCommand.cpp
        src/modules/custom/CustomSources.cpp
        src/modules/custom/CustomModule.cpp
        src/modules/custom/CustomModule.h
        src/modules/custom/ControllerCustom.cpp
        src/modules/icue/IcueAutomation.cpp
        src/modules/icue/LightingLink.cpp
        src/modules/icue/IcueSdk.cpp
        src/modules/icue/ControllerIcue.cpp
        src/modules/nvidia/NvmlReader.cpp
        src/modules/nvidia/ControllerNvidia.cpp
        src/modules/windows/media/AudioMonitor.cpp
        src/modules/windows/WindowsCollector.cpp
        src/modules/windows/lighting/DynamicLighting.cpp
        src/modules/windows/lighting/ControllerLighting.cpp
        src/modules/coretemp/CoreTempReader.cpp
        src/modules/coretemp/ControllerCoreTemp.cpp
        src/modules/hwinfo/HwinfoReader.cpp
        src/comms/TlsIdentity.cpp
        src/comms/Discovery.cpp
    )
    target_include_directories(controller_tests PRIVATE src)
    add_executable(diagnostic_crash_fixture tests/diagnostic_crash_fixture.cpp)
    target_compile_definitions(diagnostic_crash_fixture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_dependencies(controller_tests diagnostic_crash_fixture)
    add_dependencies(controller_tests icue_sdk_fixture)
    target_compile_definitions(
        controller_tests PRIVATE ICUE_TEST_DLL="$<TARGET_FILE_NAME:icue_sdk_fixture>"
    )
    target_compile_definitions(controller_tests PRIVATE WHAT_VERSION="${WHAT_RELEASE_VERSION}")
    qt_add_shaders(controller_tests test_openclose_shader
        ${WHAT_SHADER_OPTIONS}
        PREFIX "/shaders"
        BASE "qml/shaders"
        FILES qml/shaders/openclose.frag
    )
    qt_add_resources(
        controller_tests
        test_ui
        PREFIX
        "/"
        FILES
        ${WHAT_QML_FILES}
        src/modules/windows/specs/SpecsInventory.ps1
        tests/specs_inventory_fixture.ps1
        packaging/Setup.cs
        assets/logo-roof.svg
        assets/app-icon.png
        assets/app-icon.ico
        assets/update-available.svg
        assets/brands/buy-me-a-coffee.svg
        assets/brands/paypal.svg
        assets/brands/corsair.svg
        assets/brands/coretemp.svg
        assets/brands/hwinfo.png
        assets/brands/nvidia.svg
        assets/brands/windows.svg
        assets/brands/wallpaper.png
        assets/brands/beacn.svg
        assets/brands/github-sponsors.svg
        assets/Logo150.png
        assets/chevron.svg
    )
    target_link_libraries(
        controller_tests
        PRIVATE beacn_backend what_settings
                Qt6::Quick
                Qt6::QuickControls2
                Qt6::Test
                Qt6::Widgets
                Qt6::Network
                Qt6::WebSockets
                Qt6::Concurrent
                OpenSSL::Crypto
                paho-mqtt3as-static
                dwmapi
                crypt32
                version
                dnsapi
                ws2_32
                psapi
                iphlpapi
                wlanapi
                wtsapi32
                ole32
                uuid
                wevtapi
                user32
                dxgi
                tbs
                cfgmgr32
                slc
                shcore
                pdh
            powrprof
                shell32
    )
    target_compile_definitions(
        controller_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    )
    add_test(NAME controller_tests COMMAND controller_tests)
    qt_add_executable(reader_tests tests/reader_tests.cpp src/modules/hwinfo/HwinfoReader.cpp)
    target_include_directories(reader_tests PRIVATE src)
    target_link_libraries(reader_tests PRIVATE Qt6::Core)
    target_compile_definitions(reader_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_test(NAME reader_tests COMMAND reader_tests)
    qt_add_executable(
        update_tests tests/update_tests.cpp src/updater/UpdateManager.cpp
        src/updater/UpdateDownload.cpp src/updater/UpdateManager.h
    )
    target_include_directories(update_tests PRIVATE src)
    target_link_libraries(update_tests PRIVATE what_settings Qt6::Test Qt6::Network Qt6::Gui crypt32)
    target_compile_definitions(
        update_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    )
    add_test(NAME update_tests COMMAND update_tests)
    qt_add_executable(settings_tests tests/settings_tests.cpp)
    target_link_libraries(settings_tests PRIVATE what_settings Qt6::Test)
    add_test(NAME settings_tests COMMAND settings_tests)
endif()
install(TARGETS WHAT RUNTIME DESTINATION .)
# Build the helper with this target's toolchain. Copying a previous release's
# helper can silently mix architectures and leave portable builds incomplete.
add_executable(what_windows_helper WIN32 src/modules/windows/bridge/WindowsBridge.cpp
    src/app/app-icon.rc)
set_target_properties(what_windows_helper PROPERTIES OUTPUT_NAME "WHAT.WindowsHelper"
    AUTOMOC OFF AUTOUIC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_features(what_windows_helper PRIVATE cxx_std_20)
target_include_directories(what_windows_helper PRIVATE assets)
target_compile_definitions(what_windows_helper PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(what_windows_helper PRIVATE windowsapp user32)
target_link_options(what_windows_helper PRIVATE /ENTRY:wmainCRTStartup)
add_dependencies(WHAT what_windows_helper)
add_custom_command(TARGET WHAT POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:WHAT>/helpers"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:what_windows_helper>"
            "$<TARGET_FILE_DIR:WHAT>/helpers/WHAT.WindowsHelper.exe")
install(TARGETS what_windows_helper RUNTIME DESTINATION helpers)


add_executable(
    icue_automation_probe EXCLUDE_FROM_ALL
    tools/probe-icue-automation.cpp src/modules/icue/IcueAutomation.cpp
    src/modules/icue/LightingLink.cpp
)
target_include_directories(icue_automation_probe PRIVATE src)
target_link_libraries(icue_automation_probe PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

# Read-only inventory and validation probe; never applies display changes.
add_executable(
    displays_probe EXCLUDE_FROM_ALL tools/probe-displays.cpp
                                    src/modules/windows/displays/Displays.cpp
)
target_include_directories(displays_probe PRIVATE src)
target_link_libraries(displays_probe PRIVATE Qt6::Core user32 shcore)
target_compile_definitions(displays_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

add_executable(nvidia_probe tools/nvidia-probe.cpp src/modules/nvidia/NvmlReader.cpp)
target_include_directories(nvidia_probe PRIVATE src)
target_link_libraries(nvidia_probe PRIVATE Qt6::Core ole32 shell32 uuid)
target_compile_definitions(nvidia_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(beacn_probe EXCLUDE_FROM_ALL tools/beacn-probe.cpp)
target_include_directories(beacn_probe PRIVATE src)
target_link_libraries(beacn_probe PRIVATE beacn_backend)
if(TARGET beacn-volume-hook)
    add_dependencies(beacn_probe beacn-volume-hook)
endif()

# Native media-session collection uses C++/WinRT in the main app.
add_library(what_media STATIC src/modules/windows/media/MediaSessions.cpp
    src/modules/windows/media/Announcements.cpp src/modules/windows/media/Announcements.h
    src/modules/windows/notifications/RichNotifications.cpp src/modules/windows/notifications/RichNotifications.h)
target_include_directories(what_media PRIVATE src)
target_compile_features(what_media PRIVATE cxx_std_20)
target_compile_definitions(what_media PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(what_media PUBLIC Qt6::Core Qt6::Gui Qt6::Network windowsapp)
# StartupTask uses C++/WinRT, while the Qt application remains C++17.
add_library(what_startup STATIC src/app/StartupRegistration.cpp)
target_compile_features(what_startup PRIVATE cxx_std_20)
target_link_libraries(what_startup PUBLIC Qt6::Core windowsapp)

target_link_libraries(WHAT PRIVATE what_media what_haweb what_startup)
if(TARGET controller_tests)
    if(MSVC)
        # Qt's template-heavy Debug test translation unit exceeds COFF's
        # ordinary section limit; Release optimizes many of these sections away.
        target_compile_options(controller_tests PRIVATE /bigobj)
    endif()
    target_link_libraries(controller_tests PRIVATE what_media what_haweb what_startup)
endif()
