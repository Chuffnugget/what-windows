"""aqt 3.3.0 adapter for Qt 6.11.2's architecture-specific Windows repository.

Only repository selection changes; upstream download, checksum verification,
extraction and relocation remain in aqt. Remove once aqt supports this layout.
"""
from importlib.metadata import version
import sys
from aqt.archives import QtArchives
from aqt import main

if version("aqtinstall") != "3.3.0":
    raise RuntimeError("This compatibility adapter requires aqtinstall 3.3.0")

original = QtArchives._get_archives

def get_archives(self):
    if str(self.version) == "6.11.2" and self.os_name == "windows" and self.target == "desktop" and self.arch == "win64_msvc2022_64":
        return self._get_archives_base("qt6_6112/qt6_6112_msvc2022_64", self._target_packages())
    return original(self)

QtArchives._get_archives = get_archives

if __name__ == "__main__":
    sys.exit(main())
