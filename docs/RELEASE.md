# Building and publishing WHAT

## Local build

For development, run `scripts/Build-Release.cmd` from the Windows repository.
It builds the Release app and Windows helper, runs tests, and prepares a portable
runtime without a signing identity. After a successful build, run it from PowerShell:

```powershell
$build = Get-Content ./build/latest-release.json -Raw | ConvertFrom-Json
& (Join-Path $build.runtime 'WHAT.exe')
```

`scripts/Build-All.cmd` builds both x64 and ARM64 by default. On an x64 PC it
tests and packages x64, then cross-compiles and packages ARM64 without running it.
ARM64 packages are marked as not runtime-tested. Build-All checks both toolchains
and the configured signing identity before starting. The scripts stop on the first
build or packaging failure and do not upload anything or install WHAT on the build PC.

Choose the `Windows-x64-Setup.exe` file for Intel and AMD PCs, or
`Windows-arm64-Setup.exe` for Windows on ARM. Each installer output folder includes
its architecture in the name. Build-All also prints the installer for the build PC
at the end, so the last folder created is not mistaken for the only output.

For just one architecture, use `scripts/Build-All.cmd -Architecture x64` or
`scripts/Build-All.cmd -Architecture arm64`.

To run the stages separately:

```powershell
./scripts/Build-Release.ps1
./scripts/Build-Installers.ps1
```

The `.cmd` launchers run the same scripts in Windows PowerShell. For a
prerequisites-only check, use `scripts/Build-All.ps1 -ValidateOnly`. This checks
dependencies without downloading or building missing ones.

Build-All prints the current stage, task and elapsed time. Compilation has a text
progress bar based on Ninja's completed tasks, updated at five-percent milestones.
The stage count is not an estimate of remaining build time. Progress uses ordinary
console lines so it does not flash over scrolling output. Downloads, signing and
compression can be quiet,
so a status line appears every 15 seconds without tool output. This confirms the
build worker is still running; it does not prove a silent tool is making progress.
You do not need to press Enter. Ctrl+C cancels the worker and its child processes.
Console selection pause is disabled for the run and restored afterwards.

Packaging checks the source and runtime files against the compiled build. If
either has changed, run Build-Release again before Build-Installers.

## Compilation report

Build-All saves `build/compilation-report.md` and `build/compilation-report.json`
after each run, including failed builds and `-ValidateOnly`. Each run also keeps
its own report and full console transcript in `build/reports/<run>/`.

The report includes stage results and durations, selected toolchains, compiler
errors and warnings, Qt test totals and skipped tests, runtime-test status, and
installer and portable ZIP paths. Repeated diagnostic messages are grouped per
stage, with log links and line numbers. Full Qt logs are scanned too, so warnings
omitted from live test output still appear. The console and report show separate
counts for WHAT source files, tests, build configuration, release validation, and
dependencies such as OpenSSL and yaml-cpp. Unrecognised sources appear as `Other`.
Dependency warnings remain in the total and the detailed report; none are suppressed.
Qt tests have no message cap, so verbose tests cannot hide later warnings.

Qt 6.11.2 can report items still being created when an asynchronous control is
cancelled. Qt tracks this under [QTBUG-139552](https://bugreports.qt.io/browse/QTBUG-139552).
The report labels that message as `Qt` and retains the test name and full log.
Other Qt test warnings stay in `Tests`; they are not assumed to be upstream bugs.

Counts describe detected log messages. The linked logs retain multiline details
and any output the summary does not classify.

ARM64 cross builds show runtime tests as `pending`, even when compilation and
packaging succeed. A failed run keeps its original error and exit status. Earlier
successful stages remain in the report, and later stages are marked `Not run`.

## Required tools

### Native Windows ARM64

Build-Release defaults to x64; Build-All includes both architectures. For ARM64,
install the Qt 6.11.2 MSVC ARM64 kit
alongside the x64 host kit, and MSVC v143 14.44 ARM64 build tools. The target kit
must have working `host-qmake.bat`, `host-qtpaths.bat` and `qt.toolchain.cmake`.
Set `Arm64Qt` in `release.local.json` if it is not beside the x64 kit.

```powershell
./scripts/Prepare-Arm64OpenSSL.ps1
./scripts/Build-Release.ps1 -Architecture arm64
```

Build-All automatically runs the preparation script when the project-local
ARM64 OpenSSL dependency is missing or incomplete, including after clearing
`build`. You only need the explicit preparation command when running the stages
separately. It builds pinned OpenSSL 3.5.9 using verified OpenSSL source and
portable Strawberry Perl downloads under `build/dependencies`, without installing
global tools. Windows `tar` extracts only the Perl runtime; Strawberry's bundled
GCC tools are not needed. Interrupted extractions are retried on the next run.
Later builds reuse the completed dependency.

Alternatively set `Arm64OpenSSLRoot` to a native MSVC OpenSSL 3 installation with
headers, import libraries and ARM64 DLLs. An incomplete override stops the build;
the script does not replace it. Signing uses the existing x64 host OpenSSL tool,
never the ARM64 executable.

Cross builds produce a portable runtime and test executables, and verify every
deployed EXE/DLL's architecture. On an x64 host, the scripts print the staging
folder and leave its runtime tests pending. Build-All packages ARM64 with a
warning, and records `runtimeTests: pending` in the installer report. Source hashes,
runtime hashes, architecture checks and signing verification still apply. Failed
tests block packaging; x64 still requires passing runtime tests.

ARM64 runtime validation is deferred. To run it later, copy the staging folder
and matching source checkout to a Windows ARM64 PC with the test prerequisites
installed, then run:

```powershell
./scripts/Test-ReleaseRuntime.ps1 -ReleaseDirectory '<copied staging folder>'
```

For a separate ARM64 packaging run on the build PC:

```powershell
./scripts/Build-Installers.ps1 -Architecture arm64 -ReleaseDirectory '<staging folder>'
```

Use the validated staging folder if runtime tests have since passed on ARM64.

The source and runtime hashes must still match. ARM64 installers require Windows
11 22H2 or newer, which includes native .NET Framework 4.8.1 for Setup. The Qt app,
Windows helper, WebView2 loader, CRT and TLS libraries are native ARM64. Install
Microsoft's ARM64 WebView2 Runtime to use Web.

BEACN, iCUE (RGB and Automation), Core Temp, NVIDIA NVML and Wallpaper Engine are
disabled in native ARM64 WHAT, with explanations in Modules. Their configuration
and sensor selections are preserved. Windows sources, HWiNFO ARM64, Web and custom
commands remain available; commands still need tools compatible with the PC.

ARM64 uses `update-arm64.json`; x64 retains `update.json`. Asset names include
the architecture. Both can share a GitHub release, with one common source ZIP.
The current GitHub build workflow builds x64 only; Build-All builds both locally.
To publish both, collect both architecture outputs in one artifact directory before
running Publish-Release once. It refuses changes to an already published release.
An updater only accepts its own architecture. Existing x64 installs running
under emulation stay on the x64 update channel until manually replaced.

### x64 tools

- Qt 6.11.2 MSVC2022 64-bit, with Quick, Quick Controls, Network, WebSockets,
  Concurrent and Test, plus CMake and Ninja.
- Visual Studio 2022 17.14+ or Visual Studio 2026, with the C++ desktop workload
  and Windows SDK. The SDK must include RC, MakeAppx and SignTool.
- Qt's OpenSSL 3 toolkit for Windows x64. Other MSVC OpenSSL builds need headers,
  import libraries, runtime DLLs and `openssl.exe`.
- Native Windows Python 3.12+ for the integration contract tests.

Detection searches `D:\Qt`, `C:\Qt`, the user-profile Qt directory and standard
Visual Studio/Windows SDK locations. The scripts select the x64 MSVC environment;
they do not fall back to MinGW. For other locations, copy
`release.local.example.json` to `release.local.json` and set the required paths.
Empty or omitted tool settings keep automatic detection enabled. Relative paths,
such as `_local/openssl`, are resolved from the Windows repository, regardless of
the directory you launch the script from. Absolute paths are also supported for
tools installed elsewhere. Local configuration and Qt Creator settings stay out
of Git.

Clone into any directory, including paths with spaces. Project files are located
relative to the scripts or CMake source directory. Generated CMake build directories
contain absolute paths, so use a fresh build directory after moving a checkout.

The scripts do not install the compiler or Qt. The first CMake configure may
download the pinned Paho MQTT and yaml-cpp dependencies. Contract testing creates a Python
environment under `build/contract-python-win32` and installs its test dependencies
on first use. Release packaging also needs the matching Qt source archives.

In Qt Creator, select **Desktop Qt 6.11.2 MSVC2022 64bit** with an x64 MSVC
compiler. The kit name stays MSVC2022 when using Visual Studio 2026. Keep Debug
and Release in separate directories and do not reuse a MinGW build directory.
The release scripts do not edit Qt Creator's saved kit selection.
Run Build-Release once before using Qt Creator to make the separately compiled
Windows helper available, then configure the Qt Creator build.

Set `OPENSSL_ROOT_DIR` in Qt Creator's initial CMake configuration to Qt's
`Tools/OpenSSLv3/Win_x64` directory. MSYS2 OpenSSL headers cannot be used with
MSVC. When switching OpenSSL installations, clear cached `OPENSSL_*`,
`LIB_EAY_*` and `SSL_EAY_*` entries before reconfiguring.

Packaging requires the existing signing configuration in `release.local.json`.
Keep that file and private signing material out of Git.

## Output files

| Location | Contents |
| --- | --- |
| `build/release/` | Reusable compiler output |
| `build/staging/<version>/<run>/` | Compiled portable runtime, runtime-test state and packaging staging |
| `build/installers/<version>/<run>/` | Setup, update payload, portable ZIP and source archives |
| `build/logs/` | Build, packaging and test logs |
| `build/compilation-report.md` / `.json` | Latest Build-All summary |
| `build/reports/<run>/` | Saved summaries and complete Build-All console logs |
| `build/latest-release.json` | Latest successfully tested x64 build |
| `build/latest-release-arm64.json` | Latest ARM64 build, including its runtime-test status |
| `build/latest-installers.json` | Latest packaged outputs and hashes |
| `_local/` | Local signing identities; active file paths are set in `release.local.json`. Do not distribute. |

Use `WHAT-<version>-Windows-x64-Setup.exe` for installation. The portable ZIP
contains the complete runtime for manual use. `UpdatePayload.msix` and
`update.json` are used by the installed app's updater. Source ZIPs do not contain
a runnable application.

Publish the matching WHAT source ZIP, four Qt source archives and
`Qt-source-SHA256SUMS.json` alongside the binaries. See
[Rebuilding](REBUILDING.md) and [Redistribution](REDISTRIBUTION.md).

## Version and tag

Set the base version and optional beta suffix in `CMakeLists.txt`:

```cmake
project(WHAT VERSION 0.0.1 LANGUAGES C CXX RC)
set(WHAT_PRERELEASE "beta.5")
```

This example requires tag `v0.0.1-beta.5`. Increment the beta number for each new
beta. Clear the suffix for a stable release. Commit and push the intended source
before creating its matching tag. Do not reuse a published tag; rerunning an old
tag uses that tag's source and workflow.

Beta numbers range from 1 to 65535. Windows requires numeric package versions,
so beta.5 in this example becomes `0.0.1.5`; stable becomes `0.0.1.0`. WHAT compares
release versions semantically and handles the numeric revision decrease when a
user confirms moving from beta to stable through the release picker. A standalone
MSIX installation can reject the lower revision.

Stable update manifests use schema 1. Beta manifests use schema 2 with
`packageVersion` and `minimumUpdaterVersion: 2`. Older updaters cannot install
beta payloads. Turning beta updates off does not remove an installed beta.

## GitHub release workflow

The workflow is in this repository's `.github/workflows/windows-release.yml`.
It runs on pushes to `main`, pull requests targeting `main`, version tags and
manual dispatch. CI builds both x64 and ARM64 with Qt 6.11.2 MSVC2022. The x64
build runs its tests; ARM64 is cross-compiled and its runtime remains untested.
CI uses the pinned aqt installer and `scripts/aqt_qt611.py` to locate both kits.
Qt, x64 OpenSSL and Ninja archives are
extracted with the runner's existing 7-Zip through aqt's `--external` option.
`scripts/aqt-ci.ini` limits installation to one worker because parallel extraction
can fail when archives create the same directory on Windows. aqt still verifies
the archive checksums.

The x64 kit supplies the host tools for ARM64. CI uses the same ARM64 OpenSSL
preparation script as local Build-All and caches the resulting dependency.

Repository settings are separate from Git. When moving to a new repository,
create an empty `ci` environment and a `release` environment restricted to tags
matching `v*`. Set these values in `release`, using the existing signing identity:

Find these under **Settings > Environments > release**. Add the three secrets
under **Environment secrets**, and the publisher under **Environment variables**.
The `ci` environment needs no signing secrets.

| Type | Name | Value |
| --- | --- | --- |
| Secret | `SIGNING_PFX_BASE64` | Base64 of the existing password-protected PFX |
| Secret | `SIGNING_CER_BASE64` | Base64 of its matching public certificate |
| Secret | `SIGNING_PFX_PASSWORD` | The PFX password |
| Variable | `SIGNING_PUBLISHER` | `CN=WHAT` |

Never commit these values or create a replacement identity for a repository move.
No personal access token is needed. The workflow uses GitHub's automatic token
and declares its required release permissions. GitHub Free supports environment
secrets in public repositories only; making a repository private changes their
availability. Keep Actions enabled and allow the pinned actions in the workflow.

A tag build packages both architectures, then uploads them together to a draft Release.
`scripts/Publish-Release.ps1` checks the exact asset set, sizes and SHA-256 hashes
before publication. It finds drafts with `gh release view`, then lists assets
using the release's numeric ID. GitHub's REST release-by-tag endpoint only returns
published releases. The script can resume a draft but refuses to overwrite a
published release. Beta tags publish as prereleases and do not replace the stable
Latest release.

Tag releases do not depend on Actions artifact storage. Normal push and pull
request runs build and test without signing or publishing. Manual branch runs
also prepare portable and source archives, retained for five days. Diagnostic
logs are retained for seven days. These artifact uploads are optional, so a quota failure
does not fail an otherwise successful build. Build and test failures still stop
publication. The local build scripts never publish.

The installer checks that the source files still match the tested build. Python
bytecode caches are excluded from this check and from the source archive, and
the integration contract tests run with bytecode writing disabled. New or edited
source files still require a rebuild; the packaging error identifies their paths.

## Integration contract

Native builds run loopback tests against the new executable
and check its reported version. Failure prevents the build being recorded as
successful.

`tests/integration_contract` contains a pinned copy of the integration client and
tests. Windows CI uses this copy without checking out the integration repository.
Local builds also compare it with the sibling `what-integration` checkout, or
the path in `WHAT_INTEGRATION_SOURCE`, when available.

After changing a bundled integration file, run
`scripts/Sync-IntegrationContract.ps1`, review the copied files and hashes, and
commit them with the Windows change. Run local Build-All with both checkouts
before tagging them. The integration runs its own validation before publishing
its tag; no cross-repository token is required.

## Updates

WHAT checks GitHub Releases shortly after startup and every 24 hours when
background checks are enabled. Stable releases are the default. **Include beta
updates** adds prereleases; **Choose release** lets the user select an older
version. Installation and downgrade always require confirmation.

The app checks the manifest, architecture, asset URL, size and SHA-256. The
external update helper checks Windows signature trust, the installed signer,
package name, publisher, architecture and version before WHAT exits. It then
installs the MSIX and restarts the registered app. It does not import certificates
or replace the user's profile. Back up the profile before a downgrade.

Portable and Qt Creator copies can check releases but do not support automatic
MSIX installation. Update files and helper logs are stored under the profile's
`updates/<run>/` directory.

The updater only reads public releases and sends no GitHub credentials. Every
redirect is checked against the allowed GitHub hosts before it is followed.

## Development and validation

`DevelopmentBuild: true` packages as WHAT.Development with a separate profile and
no production updater.

QML is embedded in the executable. After overlaying source files from a ZIP, use
Qt Creator Rebuild if older timestamps prevent resource regeneration.

Packaging and signing fixture tests do not install software or change trust:

```powershell
./tests/release-policy-tests.ps1
./tests/release-upload-tests.ps1
```

Test installation and updates separately with the packaged app.
