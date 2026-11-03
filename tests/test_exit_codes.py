#!/usr/bin/env python3
"""CLI status regressions; run with the directory containing both executables."""

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

build = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
checks = 0


def expect(status, tool, *args, **kwargs):
    global checks
    result = subprocess.run(
        [str(build / tool), *map(str, args)], capture_output=True, **kwargs
    )
    assert result.returncode == status, (
        tool, args, result.returncode, result.stdout, result.stderr
    )
    checks += 1


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(
        ">I", zlib.crc32(kind + data)
    )


with tempfile.TemporaryDirectory(prefix="juice-exit-codes-") as directory:
    root = Path(directory)
    source = root / "good.rkt"
    source.write_text("(mes (meta (engine 'AI5)))\n")
    malformed = root / "bad.rkt"
    malformed.write_text("(mes (")
    png = root / "good.png"
    png.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 1, 8, 3, 0, 0, 0))
        + chunk(b"PLTE", bytes(48))
        + chunk(b"IDAT", zlib.compress(bytes(5)))
        + chunk(b"IEND", b"")
    )

    for tool in ("juice", "juice-img"):
        expect(0, tool, "--help")
        expect(0, tool, "--version")
        expect(1, tool)
        expect(1, tool, "--unknown")
        expect(1, tool, "-d")
        expect(1, tool, "-c")
        expect(1, tool, "-d", root / "missing.mes")
        expect(1, tool, "-c", root / "missing.rkt")
        expect(1, tool, "-d", "-o")

    expect(1, "juice", "-e", "invalid")
    expect(1, "juice", "-p", "invalid")
    expect(1, "juice", "-D", "invalid")
    expect(1, "juice-img", "-W", "invalid")
    expect(1, "juice", "-c", malformed)
    expect(1, "juice", "-c", png)
    expect(0, "juice", "-c", source)
    mes = source.with_suffix(".mes")
    original = mes.read_bytes()
    expect(1, "juice", "-c", source)
    assert mes.read_bytes() == original
    expect(0, "juice", "-c", "-f", source)
    expect(1, "juice", "-d", mes)
    expect(0, "juice", "-d", "-f", mes)
    expect(1, "juice", "-c", "-o", root / "absent" / "out.mes", source)
    expect(1, "juice", "-d", "-o", root / "absent" / "out.rkt", mes)

    # Sorting puts failures before and after good input; neither may mask the
    # failure or prevent the successful file from being processed.
    for bad_name in ("aaa.rkt", "zzz.rkt"):
        mes.unlink()
        expect(1, "juice", "-c", root / bad_name, source)
        assert mes.read_bytes() == original
    source.unlink()
    expect(1, "juice", "-d", root / "aaa.mes", mes)
    assert source.is_file()

    expect(0, "juice-img", "-c", png)
    gpc = png.with_suffix(".gpc")
    original_image = gpc.read_bytes()
    expect(1, "juice-img", "-c", png)
    assert gpc.read_bytes() == original_image
    expect(0, "juice-img", "-c", "-f", png)
    expect(1, "juice-img", "-d", gpc)
    expect(0, "juice-img", "-d", "-f", gpc)
    gpc.unlink()
    expect(1, "juice-img", "-c", root / "aaa.png", png)
    assert gpc.is_file()
    expect(1, "juice-img", "-c", "-o", root / "absent" / "out.gpc", png)

    # A stream may open successfully but fail when buffered writes are closed.
    if os.name == "posix":
        import resource
        import signal

        def reject_writes():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))

        for tool, command, input_file, output in (
            ("juice", "-c", source, "full.mes"),
            ("juice", "-d", mes, "full.rkt"),
            ("juice-img", "-c", png, "full.gpc"),
        ):
            expect(1, tool, command, "-o", root / output, input_file,
                   preexec_fn=reject_writes)

print(f"Passed {checks} exit-code checks")
