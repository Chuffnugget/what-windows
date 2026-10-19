# Rebuilding and replacing libraries

WHAT is LGPL-3.0-or-later. Publish the matching WHAT Source.zip and Qt source
archives alongside each binary release, at no additional charge. Keep those
archives available to recipients of that release.

`licenses/DEPENDENCIES.json` identifies deployed DLL versions and hashes. The
matching `qtbase`, `qtdeclarative`, `qtsvg` and `qtwebsockets` source archives are
mirrored as release assets, with SHA-256 checksums. The release scripts assume an
unmodified official Qt kit. A modified kit requires publishing its corresponding
modified source and build changes instead; do not release using mismatched sources.

Install the matching x64 or ARM64 Qt/MSVC kit, CMake, Ninja and OpenSSL development
files. ARM64 cross builds also need the x64 Qt host tools; see docs/RELEASE.md.
Include Qt Shader Tools in the kit; CMake uses `qsb` to compile WHAT's open/close effects.
If the Windows SDK's `fxc` is on PATH, the build also compiles Direct3D bytecode
so opening an effect does not need HLSL compilation at runtime.
CMake also downloads the pinned Microsoft WebView2 SDK from NuGet and links its
small static loader. No Qt WebEngine kit is needed. The separate WebView2 Runtime
must be installed to use Home Assistant Web; it is not bundled in WHAT.
Follow README.md and docs/RELEASE.md to build WHAT from Source.zip. Build replacement
Qt libraries using the matching sources/toolchain and Qt's build instructions in
those archives. Keep the ABI and architecture compatible. In the portable
distribution, close WHAT and replace the matching DLLs/plugins with your builds.
You may debug and reverse engineer the combined application to debug modifications.

Windows protects installed MSIX contents and verifies its signature. You do not
need WHAT's private signing key: use the portable build, or package your modified
build with your own signing key. Set `DevelopmentBuild` to `true` and configure
your own PFX, public certificate and password. The current packaging and Setup
scripts require the certificate subject `CN=WHAT`; this name does not grant trust
or require WHAT's private key. Explicitly trust your own certificate before
verification/installation. Development builds use the separate WHAT.Development
package and profile, with the public updater disabled. A different publisher
requires adapting the packaging checks and using Windows' `Add-AppxPackage` after
trusting that publisher. It cannot overwrite an installed production package.

Locally obtained proprietary SDK components remain subject to their own terms;
they are not relicensed by WHAT. See docs/REDISTRIBUTION.md before distributing
your build. Modified builds can omit optional vendor modules if necessary.
