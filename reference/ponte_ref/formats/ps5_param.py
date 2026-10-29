"""Leitura de sce_sys/param.json (PS5) — SPEC §1.5."""

from __future__ import annotations

import json

from ..versions import firmware_from_hex_text


def parse_param_json(data: bytes) -> dict:
    """Retorna title_id, content_id, version, title, required_firmware (podem ser None).

    Lança ValueError se não for JSON de objeto.
    """
    obj = json.loads(data.decode("utf-8-sig"))
    if not isinstance(obj, dict):
        raise ValueError("param.json não é um objeto")

    title = None
    lp = obj.get("localizedParameters")
    if isinstance(lp, dict):
        default = lp.get("defaultLanguage")
        entry = lp.get(default) if isinstance(default, str) else None
        if isinstance(entry, dict):
            title = entry.get("titleName")
        if not title:
            for v in lp.values():
                if isinstance(v, dict) and v.get("titleName"):
                    title = v["titleName"]
                    break

    def s(key):
        v = obj.get(key)
        return v if isinstance(v, str) and v else None

    return {
        "title_id": s("titleId"),
        "content_id": s("contentId"),
        "version": s("contentVersion"),
        "title": title if isinstance(title, str) and title else None,
        "required_firmware": firmware_from_hex_text(obj.get("requiredSystemSoftwareVersion")),
    }
