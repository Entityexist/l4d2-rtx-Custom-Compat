from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
checks = {
    "L4D1 game id": (root / "src/source_compat.hpp", "left_4_dead,"),
    "L4D1 bootstrap mode": (root / "src/source_compat.hpp", "native_l4d1_bootstrap"),
    "L4D1 executable detection": (root / "src/source_compat.inl", 'exe == "left4dead.exe"'),
    "L4D1 known SHA-1": (root / "src/source_compat.inl", "098d07422acc07d560315fafebd021ea661121cd"),
    "No-hook bootstrap resolver": (root / "src/game/l4d1.cpp", "No binary hooks will be installed"),
    "Surface030 fallback": (root / "src/components/modules/interfaces.cpp", '"VGUI_Surface030"'),
    "Player manager null guard": (root / "src/components/modules/interfaces.cpp", "PlayerInfoManager002 unavailable"),
    "Dedicated loader branch": (root / "src/components/loader.cpp", "uses_l4d1_bootstrap()"),
    "Read-only audit call": (root / "src/main.cpp", "l4d1::init_bootstrap_addresses()"),
    "L4D1_ROOT build route": (root / "premake5.lua", 'os.getenv("L4D1_ROOT")'),
}
failed = []
for name, (path, needle) in checks.items():
    text = path.read_text(encoding="utf-8")
    if needle not in text:
        failed.append(name)
        print(f"FAIL: {name}")
    else:
        print(f"PASS: {name}")

loader = (root / "src/components/loader.cpp").read_text(encoding="utf-8")
branch = loader.split("if (source_compat::uses_l4d1_bootstrap())", 1)[1].split("// Safe Generic Source core", 1)[0]
for forbidden in ("MH_EnableHook", "game_settings", "remix_vars", "main_module", "model_render"):
    if forbidden in branch and forbidden not in ("game_settings", "remix_vars"):
        failed.append(f"forbidden L4D1 branch token: {forbidden}")
# game_settings/remix_vars are allowed only in comments explaining why they are excluded.
if "register_component<game_settings>" in branch or "register_component<remix_vars>" in branch:
    failed.append("L4D2 settings components registered in L4D1 branch")

if failed:
    print("\nL4D1 bootstrap validation FAILED:")
    for item in failed: print(" -", item)
    sys.exit(1)
print("\nL4D1 bootstrap validation PASSED")
