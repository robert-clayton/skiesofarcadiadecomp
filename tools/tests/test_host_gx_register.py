"""The optional GX register service: typed widths, preflight and refusal with no side effects."""
import os
import shutil
import subprocess
from pathlib import Path
import pytest

@pytest.mark.skipif(shutil.which("gcc") is None, reason="requires GCC on Linux")
def test_typed_register_service(tmp_path):
    root=Path(__file__).resolve().parents[2]
    exe=tmp_path/"gx-register"
    out=subprocess.run(["gcc", "-std=c11", "-O2", "-ffunction-sections", "-fdata-sections",
        str(root/"tools/tests/host_gx_register_probe.c"), "-Wl,--gc-sections", "-lm", "-o", str(exe)], capture_output=True, text=True, timeout=60)
    assert out.returncode==0, out.stdout+out.stderr
    run=subprocess.run([str(exe)], capture_output=True,text=True,timeout=60)
    assert run.returncode==0,run.stdout+run.stderr
    assert "75 valid registers, 75 reads, 75 writes" in run.stdout

@pytest.mark.skipif(not os.environ.get("SOA_TEST_GX_RUNTIME"), reason="requires a built GX runtime to test its real device model")
def test_real_gx_model(tmp_path):
    root=Path(__file__).resolve().parents[2]
    exe=tmp_path/"gx-device"
    out=subprocess.run(["gcc","-std=c11","-O2","-ffunction-sections","-fdata-sections","-I",str(root/"runtime"),
        str(root/"tools/tests/host_gx_register_device_probe.c"),"-Wl,--gc-sections","-ldl","-lm","-o",str(exe)],capture_output=True,text=True,timeout=60)
    assert out.returncode==0,out.stdout+out.stderr
    run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60)
    assert run.returncode==0,run.stdout+run.stderr
    assert "pointer advance and wrap PASS" in run.stdout
