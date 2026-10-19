"""Locate Qt 6.11.2 MSVC archives and relocate its ARM64 host launchers.

Download, checksum verification and extraction remain in aqt. Remove this adapter
once aqt supports the new repository layout and host-prefixed Windows scripts.
"""
from importlib.metadata import version
import sys
from aqt.archives import QtArchives
from aqt.updater import Updater
from aqt import main

if version("aqtinstall") != "3.3.0":
    raise RuntimeError("This compatibility adapter requires aqtinstall 3.3.0")

original = QtArchives._get_archives
original_patch_scripts = Updater.patch_qt_scripts

def get_archives(self):
    repositories = {
        "win64_msvc2022_64": "qt6_6112/qt6_6112_msvc2022_64",
        "win64_msvc2022_arm64_cross_compiled": "qt6_6112/qt6_6112_msvc2022_arm64_cross_compiled",
    }
    if (str(self.version) == "6.11.2" and self.os_name == "windows"
            and self.target == "desktop" and self.arch in repositories):
        return self._get_archives_base(repositories[self.arch], self._target_packages())
    return original(self)

QtArchives._get_archives = get_archives


def patch_qt_scripts(self, base_dir, version_dir, os_name, desktop_arch_dir, qt_version):
    if (version_dir != "6.11.2" or not os_name.startswith("windows")
            or self.prefix.name != "msvc2022_arm64"):
        return original_patch_scripts(self, base_dir, version_dir, os_name, desktop_arch_dir, qt_version)

    # Qt 6.11 ships ARM64 qmake.exe and separate host-*.bat launchers. Older aqt
    # looks for qmake.bat, which no longer exists. Relative, quoted host paths
    # also keep these launchers valid when a cached kit is moved to another runner.
    for name in ("qmake", "qmake6", "qtpaths", "qtpaths6"):
        script = self.prefix / "bin" / f"host-{name}.bat"
        if not script.is_file():
            raise FileNotFoundError(f"Qt ARM64 host launcher missing: {script}")
        executable = name if name.endswith("6") else name + "6"
        host = f"%~dp0..\\..\\{desktop_arch_dir}\\bin\\{executable}.exe"
        script.write_text(
            f'@echo off\n"{host}" -qtconf "%~dp0target_qt.conf" %*\n',
            encoding="utf-8",
        )
        self.logger.info("Patched ARM64 host launcher %s", script)


Updater.patch_qt_scripts = patch_qt_scripts

if __name__ == "__main__":
    sys.exit(main())
