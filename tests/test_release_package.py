"""验证发布标签约束、分发内容完整性，以及打包失败时保留原有产物。"""

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

REPOSITORY = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "package_release", REPOSITORY / "scripts/package-release.py")
RELEASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RELEASE)


class ReleasePackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shotlet release ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.output = self.root / "dist"
        self.binary = self.build / "Release/shotlet.exe"
        self.binary.parent.mkdir(parents=True)
        self.binary.write_bytes(b"release executable fixture")
        (self.root / "CMakeLists.txt").write_text(
            "project(shotlet VERSION 1.2.3 LANGUAGES CXX C)\n", encoding="utf-8")
        for source, _ in RELEASE.LICENSE_FILES:
            license_file = self.root / source
            license_file.parent.mkdir(parents=True, exist_ok=True)
            license_file.write_text(f"License fixture: {source}\n", encoding="utf-8")
        self.root_patch = patch.object(RELEASE, "ROOT", self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)

    def test_tag_must_match_application_version(self):
        self.assertEqual(RELEASE.project_version("v1.2.3"), "1.2.3")
        self.assertEqual(RELEASE.project_version(""), "1.2.3")
        for tag in ("v1.2.4", "v1.2.3-beta.1", "v1.2.3/../../other"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                RELEASE.project_version(tag)

    def test_archive_includes_runtime_licenses_and_valid_checksum(self):
        resource = self.build / "Release/resources/中文.txt"
        resource.parent.mkdir()
        resource.write_text("运行时资源", encoding="utf-8")
        RELEASE.package_release(self.build, self.output, "windows-x64", "1.2.3")
        name = "shotlet-1.2.3-windows-x64"
        archive = self.output / f"{name}.zip"
        with zipfile.ZipFile(archive) as package:
            self.assertIsNone(package.testzip())
            self.assertEqual(package.read(f"{name}/shotlet.exe"), self.binary.read_bytes())
            self.assertEqual(package.read(f"{name}/resources/中文.txt").decode(), "运行时资源")
            for _, destination in RELEASE.LICENSE_FILES:
                self.assertIn(f"{name}/{destination}", package.namelist())
        checksum = archive.with_suffix(".zip.sha256").read_text(encoding="utf-8")
        self.assertEqual(checksum, f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n")

    def test_missing_license_preserves_previous_package(self):
        self.output.mkdir()
        archive = self.output / "shotlet-1.2.3-windows-x64.zip"
        archive.write_bytes(b"previous valid package")
        (self.root / "imgui/LICENSE.txt").unlink()
        with self.assertRaises(FileNotFoundError):
            RELEASE.package_release(self.build, self.output, "windows-x64", "1.2.3")
        self.assertEqual(archive.read_bytes(), b"previous valid package")
        self.assertEqual(list(self.output.iterdir()), [archive])


if __name__ == "__main__":
    unittest.main()
