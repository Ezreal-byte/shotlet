"""验证依赖补丁可重复、并发应用，且拒绝覆盖不兼容的本地内容。"""

import concurrent.futures
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


PATCH_TEST_TIMEOUT_SECONDS = 30
REPOSITORY = Path(__file__).resolve().parents[1]
SDL_SOURCE = Path(sys.argv.pop(1))
CMAKE_EXECUTABLE = sys.argv.pop(1)
GIT_EXECUTABLE = sys.argv.pop(1)
PATCH = REPOSITORY / "cmake/patches/disable-sdl-dynamic-api.patch"


class CmakePatchTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shotlet patch ")
        self.addCleanup(self.temporary.cleanup)
        self.source = Path(self.temporary.name)
        self.header = self.source / "src/dynapi/SDL_dynapi.h"
        self.header.parent.mkdir(parents=True)
        self.header.write_bytes((SDL_SOURCE / "src/dynapi/SDL_dynapi.h").read_bytes())
        # 使用真实上游文件，允许测试运行在补丁已经应用过的工作目录中。
        reverse = [GIT_EXECUTABLE, "apply", "--reverse"]
        check = subprocess.run(reverse + ["--check", str(PATCH)], cwd=self.source,
                               capture_output=True, timeout=PATCH_TEST_TIMEOUT_SECONDS)
        if check.returncode == 0:
            subprocess.run(reverse + [str(PATCH)], cwd=self.source, check=True,
                           capture_output=True, timeout=PATCH_TEST_TIMEOUT_SECONDS)
        self.original = self.header.read_bytes()
        self.probe = self.source / "apply.cmake"
        self.probe.write_text(
            'include("${SHOTLET_PATCH_MODULE}")\n'
            'shotlet_apply_patch("${SHOTLET_PATCH_SOURCE}" "${SHOTLET_PATCH_FILE}")\n',
            encoding="utf-8",
        )

    def apply_patch(self):
        return subprocess.run(
            [CMAKE_EXECUTABLE, f"-DGIT_EXECUTABLE={GIT_EXECUTABLE}",
             f"-DSHOTLET_PATCH_MODULE={REPOSITORY / 'cmake/ApplyPatch.cmake'}",
             f"-DSHOTLET_PATCH_SOURCE={self.source}", f"-DSHOTLET_PATCH_FILE={PATCH}",
             "-P", str(self.probe)],
            capture_output=True, text=True, encoding="utf-8", timeout=PATCH_TEST_TIMEOUT_SECONDS,
        )

    def test_application_is_idempotent(self):
        first = self.apply_patch()
        self.assertEqual(first.returncode, 0, first.stderr)
        patched = self.header.read_bytes()
        self.assertIn(b"#define SDL_DYNAMIC_API 0", patched)
        self.assertNotIn(b"#define SDL_DYNAMIC_API 1", patched)
        second = self.apply_patch()
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(self.header.read_bytes(), patched)

    def test_incompatible_source_is_preserved(self):
        changed = self.original.replace(b"#define SDL_DYNAMIC_API 1", b"#define SDL_DYNAMIC_API 2")
        self.assertNotEqual(changed, self.original)
        self.header.write_bytes(changed)
        result = self.apply_patch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("补丁与依赖源码不匹配", result.stderr)
        self.assertEqual(self.header.read_bytes(), changed)

    def test_concurrent_configuration_applies_patch_once(self):
        concurrent_configurations = 2
        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrent_configurations) as executor:
            results = list(executor.map(lambda _: self.apply_patch(), range(concurrent_configurations)))
        for result in results:
            self.assertEqual(result.returncode, 0, result.stderr)
        expected = self.original.replace(b"#define SDL_DYNAMIC_API 1", b"#define SDL_DYNAMIC_API 0")
        self.assertEqual(self.header.read_bytes(), expected)


if __name__ == "__main__":
    unittest.main()
