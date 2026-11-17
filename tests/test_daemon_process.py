"""使用真实子进程验证单实例、脱离终端和参数传递，不启动桌面窗口。"""

import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


PROBE = Path(sys.argv.pop(1)).resolve()


class DaemonProcessTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="shotlet_daemon_")
        self.report = Path(self.directory.name) / "中文 参数.bin"

    def tearDown(self):
        # 失败时也让仍在等待的测试子进程退出。
        for report in Path(self.directory.name).glob("*.bin"):
            Path(str(report) + ".release").touch()
        for report in Path(self.directory.name).glob("*.bin"):
            self.wait_finished(report)
        self.directory.cleanup()
        if os.name != "nt":
            lock = Path(f"/tmp/shotlet-daemon-{os.getuid()}") / (Path(self.directory.name).name + ".lock")
            # 测试进程已退出后清理独有名称，生产代码不会删除锁文件。
            lock.unlink(missing_ok=True)

    def command(self, *arguments, report=None):
        return [str(PROBE), "--probe-output", str(report or self.report), *arguments]

    def read_report(self, report=None):
        report = report or self.report
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if report.exists():
                fields = report.read_bytes().split(b"\0")
                if len(fields) >= 7 and fields[-1] == b"":
                    return [int(field) for field in fields[:5]], [
                        field.decode("utf-8") for field in fields[5:-1]
                    ]
            time.sleep(0.01)
        self.fail("子进程没有写出完整报告")

    def wait_finished(self, report=None):
        finished = Path(str(report or self.report) + ".finished")
        deadline = time.monotonic() + 5
        while not finished.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(finished.exists(), "脱离终端后子进程未完成工作")

    def start_held_worker(self, *arguments):
        result = subprocess.run(self.command("--daemon", "--probe-wait", *arguments), capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"")
        return self.read_report()[0][0]

    def assert_skipped(self, *arguments):
        alternate = self.report.with_name("重复.bin")
        result = subprocess.run(self.command(*arguments, report=alternate), capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("已在后台运行", result.stdout.decode("utf-8"))
        self.assertFalse(alternate.exists(), "重复启动进入了应用入口")

    def test_repeated_daemon_and_foreground_are_skipped(self):
        self.start_held_worker()
        self.assert_skipped("--daemon")
        self.assert_skipped("-d")
        self.assert_skipped("--daemon", "--foreground")

    def test_duplicate_launch_honors_explicit_language_before_application_entry(self):
        self.start_held_worker("--language", "en")
        alternate = self.report.with_name("英文重复.bin")
        result = subprocess.run(self.command("--daemon", "--language=en", report=alternate),
                                capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("already running in the background", result.stdout.decode("utf-8"))
        self.assertFalse(alternate.exists())

    def test_foreground_daemon_also_blocks_detached_launch(self):
        launcher = subprocess.Popen(self.command("--daemon", "--foreground", "--probe-wait"))
        try:
            metadata, _ = self.read_report()
            self.assertEqual(metadata[0], launcher.pid)
            self.assert_skipped("--daemon")
        finally:
            Path(str(self.report) + ".release").touch()
            self.assertEqual(launcher.wait(timeout=3), 37)

    def test_simultaneous_launchers_create_exactly_one_worker(self):
        reports = [self.report.with_name(f"并发{i}.bin") for i in range(8)]
        launchers = [subprocess.Popen(self.command("--daemon", "--probe-wait", report=report),
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE) for report in reports]
        winners = []
        for report, launcher in zip(reports, launchers):
            stdout, stderr = launcher.communicate(timeout=3)
            self.assertEqual(launcher.returncode, 0, stderr)
            if stdout == b"":
                winners.append(report)
            else:
                self.assertIn("已在后台运行", stdout.decode("utf-8"))
        self.assertEqual(len(winners), 1)
        self.read_report(winners[0])
        self.assertEqual([report for report in reports if report.exists()], winners)

    def test_daemon_can_restart_after_exit_with_existing_lock_file(self):
        self.start_held_worker()
        Path(str(self.report) + ".release").touch()
        self.wait_finished()
        alternate = self.report.with_name("重新启动.bin")
        # 完成标记在入口返回前写入，允许锁析构完成后重新启动。
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            result = subprocess.run(self.command("--daemon", "--foreground", report=alternate),
                                    capture_output=True, timeout=3)
            if result.returncode == 37:
                break
            time.sleep(0.01)
        self.assertEqual(result.returncode, 37, result.stderr)
        self.read_report(alternate)

    @unittest.skipIf(os.name == "nt", "POSIX 异常终止测试")
    def test_lock_is_released_after_sigkill(self):
        worker = self.start_held_worker()
        os.kill(worker, signal.SIGKILL)
        # 被杀的工作进程无法写完成标记，供清理逻辑辨识。
        Path(str(self.report) + ".finished").touch()
        alternate = self.report.with_name("异常后重启.bin")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            result = subprocess.run(self.command("--daemon", "--foreground", report=alternate),
                                    capture_output=True, timeout=3)
            if result.returncode == 37:
                break
            time.sleep(0.01)
        self.assertEqual(result.returncode, 37, result.stderr)
        self.read_report(alternate)

    def test_normal_capture_can_run_while_daemon_is_active(self):
        self.start_held_worker()
        alternate = self.report.with_name("普通截图.bin")
        result = subprocess.run(self.command(report=alternate), capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 37, result.stderr)
        self.read_report(alternate)

    def test_daemon_returns_before_worker_and_preserves_arguments(self):
        arguments = ["--daemon", "", '中文 路径\\尾部\\', '引号"和值\n下一行']
        command = self.command(*arguments)
        launcher = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            # 子进程不继承管道；启动程序返回后 communicate 应立即结束。
            stdout, stderr = launcher.communicate(timeout=3)
            self.assertEqual(launcher.returncode, 0, stderr.decode(errors="replace"))
            self.assertEqual(stdout, b"")
            self.assertEqual(stderr, b"")
            self.assertFalse(Path(str(self.report) + ".finished").exists())
            metadata, forwarded = self.read_report()
            self.assertNotEqual(metadata[0], launcher.pid)
            self.assertEqual(metadata[2:4], [1, 0])
            if os.name != "nt":
                self.assertEqual(metadata[1], metadata[0])
            self.assertEqual(forwarded, command)
            self.wait_finished()
        finally:
            if launcher.poll() is None:
                launcher.kill()
                launcher.wait()

    def test_short_daemon_option(self):
        result = subprocess.run(self.command("-d"), capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        metadata, forwarded = self.read_report()
        self.assertEqual(metadata[2:4], [1, 0])
        self.assertIn("-d", forwarded)
        self.wait_finished()

    def test_foreground_keeps_process_and_exit_status(self):
        command = self.command("--foreground", "--daemon")
        launcher = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        launcher.communicate(timeout=3)
        self.assertEqual(launcher.returncode, 37)
        metadata, forwarded = self.read_report()
        self.assertEqual(metadata[0], launcher.pid)
        self.assertEqual(forwarded, command)
        self.wait_finished()

    def test_normal_mode_keeps_process_and_exit_status(self):
        command = self.command()
        launcher = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        launcher.communicate(timeout=3)
        self.assertEqual(launcher.returncode, 37)
        metadata, forwarded = self.read_report()
        self.assertEqual(metadata[0], launcher.pid)
        self.assertEqual(forwarded, command)
        self.wait_finished()

    @unittest.skipIf(os.name == "nt", "POSIX 文件描述符测试")
    def test_daemon_preserves_lock_when_standard_streams_are_closed(self):
        def close_streams():
            for descriptor in range(3):
                os.close(descriptor)

        result = subprocess.run(self.command("--daemon", "--probe-wait"),
                                capture_output=True, preexec_fn=close_streams, timeout=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        metadata, _ = self.read_report()
        self.assertEqual(metadata[2:4], [1, 0])
        self.assert_skipped("--daemon")

    @unittest.skipIf(os.name == "nt", "POSIX 文件描述符测试")
    def test_daemon_closes_extra_inherited_descriptors(self):
        with open(self.report.parent / "继承描述符.txt", "w") as inherited:
            result = subprocess.run(
                self.command("--daemon", f"--probe-inherited-fd={inherited.fileno()}"),
                pass_fds=(inherited.fileno(),), capture_output=True, timeout=3,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            metadata, _ = self.read_report()
            self.assertEqual(metadata[4], 0)
            self.wait_finished()

    @unittest.skipIf(os.name == "nt", "POSIX 终端挂断测试")
    def test_daemon_survives_terminal_closure(self):
        import pty

        command = self.command("--daemon")
        pid, terminal = pty.fork()
        if pid == 0:
            os.execv(command[0], command)
        try:
            metadata, _ = self.read_report()
            self.assertNotEqual(metadata[1], pid)
            self.assertEqual(metadata[2:4], [1, 0])
            _, status = os.waitpid(pid, 0)
            self.assertTrue(os.WIFEXITED(status))
            self.assertEqual(os.WEXITSTATUS(status), 0)
            # 启动命令返回后挂断真实伪终端，子进程应继续工作。
            os.close(terminal)
            terminal = -1
            self.wait_finished()
        finally:
            if terminal >= 0:
                os.close(terminal)


if __name__ == "__main__":
    unittest.main()
