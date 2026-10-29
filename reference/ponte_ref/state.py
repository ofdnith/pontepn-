"""Cruzamento do catálogo com o que já está no console (SPEC §5.7)."""

from __future__ import annotations

from .library import Item
from .profiles import Profile, select_profile
from .versions import compare_firmware, compare_versions


def profile_warnings(kind: str, profile: Profile | None, guessed: bool) -> list[str]:
    if profile is None:
        return []
    out = ["profile_guessed"] if guessed else []
    status, _ = profile.status(kind)
    if status == "experimental":
        out.append("experimental_format")
    elif status == "unknown":
        out.append("unknown_compatibility")
    return out


def item_state(item: Item, installed: list[dict], firmware: str | None,
               profile: Profile | None) -> tuple[str, str | None]:
    i = item.ident
    if profile is not None:
        status, note = profile.status(i.kind)
        if status == "blocked":
            return "incompatible", note or "formato não suportado neste firmware"
    if i.required_firmware and firmware and compare_firmware(i.required_firmware, firmware) == 1:
        return "incompatible", f"exige firmware {i.required_firmware} ou superior"
    if not i.title_id:
        return "new", None
    same = [e for e in installed
            if str(e.get("title_id", "")).upper() == i.title_id.upper() and e.get("content") == i.content]
    if not same:
        return "new", None
    if any(compare_versions(i.version, e.get("version"), i.platform) <= 0 for e in same):
        return "installed", None
    return "update_available", None


def annotate(items: list[Item], console: dict, profiles: list[Profile]) -> list[dict]:
    platform = console.get("platform")
    firmware = console.get("firmware")
    installed = [e for e in console.get("installed", []) if isinstance(e, dict)]
    profile, guessed = select_profile(profiles, platform, firmware)
    out = []
    for it in items:
        d = it.summary()
        d["warnings"] = list(dict.fromkeys(d["warnings"] + profile_warnings(it.ident.kind, profile, guessed)))
        d["state"], d["state_reason"] = item_state(it, installed, firmware, profile)
        out.append(d)
    return out
