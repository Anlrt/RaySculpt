"""Check the opt-in native worker with the real Windows trace engine."""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORKER = os.environ.get("RAYSCULPT_WORKER")
ENGINE = ROOT / "build/Release/caustic_cli.exe"


@unittest.skipUnless(os.name == "nt" and WORKER, "native Windows worker not enabled")
class WorkerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)
        self.settings = self.folder / "scene.txt"
        manifest = json.loads((ROOT / "tests/fixtures/manifest.json").read_text(encoding="utf-8"))
        self.base_settings = manifest["cases"]["flat"]["settings"]

    def start(self, rays, *, invalid=False, over_memory_budget=False, memory_limit=None):
        values = self.base_settings.split()
        values[3] = str(rays)
        if over_memory_budget:
            values[16] = "32"
            receiver = values[17:]
            receiver[-2:] = ["2048","2048"]
            values[17:] = receiver * 32
        if memory_limit is not None:
            values[-2:] = ["1024","1024"]
        self.settings.write_text(" ".join(values), encoding="ascii")
        output = self.folder / "result"
        request = dict(version=1, job_id="worker-test", revision=42, task="trace", output=str(output),
                       settings=str(self.settings if not invalid else self.folder/"absent.txt"), profile="-")
        if memory_limit is not None:
            request["memory_limit_bytes"]=memory_limit
        child = subprocess.Popen([WORKER, ENGINE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, encoding="utf-8", text=True)
        self.addCleanup(self.stop, child)
        child.stdin.write(json.dumps(request)+"\n")
        child.stdin.flush()
        progress = self.read(child, "progress")
        return child, output, progress["detail"]["pid"]

    @staticmethod
    def stop(child):
        if child.poll() is None:
            child.kill()
        child.communicate(timeout=5)

    def read(self, child, event):
        line = child.stdout.readline()
        self.assertTrue(line, "worker closed the protocol stream")
        message = json.loads(line)
        self.assertEqual((message["event"], message["job_id"], message["revision"]),
                         (event, "worker-test", 42))
        return message

    def test_failure_followed_by_success(self):
        child, output, _ = self.start(16_000, invalid=True)
        self.read(child,"error")
        self.assertNotEqual(child.wait(timeout=5),0)
        self.assertFalse(output.exists())
        child, output, _ = self.start(16_000)
        self.assertEqual(self.read(child,"progress")["detail"]["stage"],"publishing")
        self.read(child,"result")
        self.assertEqual(child.wait(timeout=5),0)
        self.assertEqual(json.loads((output/"report.json").read_text())["rays"],16_000)

    def test_cancel_during_trace(self):
        child, output, _ = self.start(200_000_000)
        child.stdin.write(json.dumps(dict(version=1, job_id="worker-test", revision=42, event="cancel"))+"\n")
        child.stdin.flush()
        self.read(child,"cancelled")
        self.assertEqual(child.wait(timeout=5),0)
        self.assertFalse(output.exists())

    def test_memory_budget_error(self):
        child, output, _ = self.start(16_000, over_memory_budget=True)
        self.assertEqual(self.read(child,"error")["detail"]["code"],"memory")
        self.assertNotEqual(child.wait(timeout=5),0)
        self.assertFalse(output.exists())

    def test_windows_job_memory_limit_is_enforced(self):
        child, output, _ = self.start(16_000,memory_limit=32*1024*1024)
        self.assertEqual(self.read(child,"error")["detail"]["code"],"memory")
        self.assertNotEqual(child.wait(timeout=5),0)
        self.assertFalse(output.exists())

    def test_closing_worker_terminates_engine(self):
        kernel = ctypes.WinDLL("kernel32",use_last_error=True)
        kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
        kernel.OpenProcess.restype=wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes=[wintypes.HANDLE,wintypes.DWORD]
        kernel.CloseHandle.argtypes=[wintypes.HANDLE]
        child, output, engine_pid = self.start(200_000_000)
        engine = kernel.OpenProcess(0x00100000,False,engine_pid)
        self.assertTrue(engine)
        try:
            child.kill()
            child.wait(timeout=5)
            self.assertEqual(kernel.WaitForSingleObject(engine,5000),0)
            self.assertFalse(output.exists())
        finally:
            kernel.CloseHandle(engine)
