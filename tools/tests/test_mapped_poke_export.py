"""Host-only callback export: no disc data or executable configuration is read."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from soa import seam  # noqa: E402


def test_host_exports_the_optional_mapped_poke_hook():
    empty = seam.Seam(runtime=(), libc=(), twins=())
    assert "    soa_core_set_memory;" in seam.version_script(empty, (), host=True)
    assert "soa_core_set_memory" not in seam.version_script(empty, (), host=False)
