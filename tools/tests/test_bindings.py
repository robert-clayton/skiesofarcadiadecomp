"""The binding lists: config/hle.txt, hooks.txt, savepoints.txt and trace.txt.

All four go through tools/soa/hle.py load_bindings, and all four are baked
into the translated C, so a line the loader does not read is a function the
runtime was meant to answer that the recompiled code answers instead, a hook
or a savepoint that is not there, or a tracepoint that never fires -- and a
full recompile later, nothing says so. The loader used to skip any line it
could not match: `0X80005520`, `0x8000552` and `80005520` each vanished with
no error and no count, which CLAUDE.md carried as a known silent trap. A
second entry for an address replaced the first just as quietly.
"""

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from soa.hle import load_bindings  # noqa: E402

BINDING_FILES = ("hle.txt", "hooks.txt", "savepoints.txt", "trace.txt", "resumepoints.txt")


def write(tmp_path: Path, body: str) -> Path:
    path = tmp_path / "hle.txt"
    path.write_text(
        "# a comment\n\n0x80005434  memset      # src/sdk/msl/mem.c\n" + body, encoding="utf-8"
    )
    return path


def test_a_well_formed_file_loads(tmp_path):
    """What a good file means, so the refusals below are not refusing it."""
    path = write(tmp_path, "   0x8025F1D8\tstrlen\n0x8022AB08  CacheStore@r7   # note\n")
    assert load_bindings(path) == {
        0x80005434: "memset",
        0x8025F1D8: "strlen",
        0x8022AB08: "CacheStore@r7",
    }
    assert load_bindings(tmp_path / "absent.txt") == {}  # a list nobody wrote binds nothing


@pytest.mark.parametrize(
    "line",
    [
        "0X80005520  memcpy",  # upper-case X
        "0x8000552  memcpy",  # seven digits
        "80005520  memcpy",  # no 0x
        "0x800055200  memcpy",  # nine digits
        "0x80005520",  # no name
        "0x80005520memcpy",  # no space between them
        "0x80005520  memcpy memmove",  # a second name, which only the first of would bind
        "memcpy  0x80005520",  # the columns the wrong way round
    ],
)
def test_a_malformed_line_is_an_error_that_says_where(tmp_path, line):
    path = write(tmp_path, f"{line}   # the note survives\n")
    with pytest.raises(ValueError, match=r"hle\.txt:4:"):
        load_bindings(path)


def test_a_second_entry_for_an_address_is_an_error_naming_both(tmp_path):
    path = write(tmp_path, "0x80005520  memcpy\n0x80005434  memset_again\n")
    with pytest.raises(ValueError, match=r"hle\.txt:5: 0x80005434 .*memset at line 3"):
        load_bindings(path)


def test_the_same_address_in_either_case_is_the_same_address(tmp_path):
    path = write(tmp_path, "0x8000552a  a\n0x8000552A  b\n")
    with pytest.raises(ValueError, match="already bound"):
        load_bindings(path)


@pytest.mark.parametrize("name", BINDING_FILES)
def test_every_line_of_every_real_list_is_an_entry(name):
    """Each committed list loads, and loads every entry it has: the count of
    non-blank, non-comment lines is the count of bindings. Not a number
    written down here, which would rot the day someone adds a tracepoint."""
    path = ROOT / "config" / name
    lines = [
        line
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.split("#", 1)[0].strip()
    ]
    assert lines, f"config/{name} binds nothing"
    assert len(load_bindings(path)) == len(lines)
