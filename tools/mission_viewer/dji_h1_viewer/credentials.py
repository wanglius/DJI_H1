"""Local credential loading; real values live only in git-ignored files."""

from __future__ import annotations

import json
from pathlib import Path
import re


class CredentialError(RuntimeError):
    """A required local credential is missing or malformed."""


def baidu_map_credential_path() -> Path:
    return (Path(__file__).resolve().parent.parent / "credentials" /
            "baidu_map.local.json")


def load_baidu_map_ak(path: str | Path | None = None) -> str:
    """Load and validate the local Baidu browser AK without logging it."""

    source = Path(path) if path is not None else baidu_map_credential_path()
    try:
        with source.open("r", encoding="utf-8") as stream:
            document = json.load(stream)
    except FileNotFoundError as exc:
        raise CredentialError(
            f"Baidu Map credential not found: {source.name}; copy the example "
            "file to baidu_map.local.json") from exc
    except (OSError, json.JSONDecodeError) as exc:
        raise CredentialError(
            f"Cannot read Baidu Map credential: {source.name}") from exc
    ak = document.get("ak") if isinstance(document, dict) else None
    if not isinstance(ak, str) or not re.fullmatch(r"[A-Za-z0-9_-]{10,128}", ak):
        raise CredentialError(
            f"Baidu Map credential has an invalid AK field: {source.name}")
    return ak
