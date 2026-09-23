import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).parents[1]


def test_dpc_header_supports_rsp_recompiler_macro_names():
    text = (ROOT / "include/rsp_dpc_macros.h").read_text()
    assert "#define SET_DPC_START" in text
    assert "#define SET_DPC_END" in text


def test_musyx_fixup_emits_a_well_formed_completion_comment(tmp_path):
    generated = tmp_path / "musyx_audio_recompiled.c"
    generated.write_text(
        '#include "librecomp/rsp.hpp"\n'
        "RspExitReason musyx_audio() {\n"
        "    RSP rsp{};\n"
        "do_indirect_jump:\n"
        "    return RspExitReason::UnhandledJumpTarget;\n"
        "}\n"
    )
    subprocess.run(
        [sys.executable, str(ROOT / "tools/fixup_factor5_ucode.py"), str(generated)],
        check=True,
    )
    text = generated.read_text()
    assert "/* fixup: musyx runaway cap: top-level jr $ra (r31=0) = task complete */" in text
    assert "*/ top-level" not in text
