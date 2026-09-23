import tomllib
from pathlib import Path

ROOT = Path(__file__).parents[1]


def test_pause_label_hook_belongs_to_containing_function():
    config = tomllib.loads((ROOT / "rogue_squadron.toml").read_text())
    matches = [
        hook
        for hook in config["patches"]["hook"]
        if hook.get("before_vram") == 0x800C3550
    ]
    assert len(matches) == 1
    assert matches[0]["func"] == "func_800C30C8"
