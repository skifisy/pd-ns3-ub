from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

from . import repo_root


QUERY_PROGRAM = "scratch/ub-quick-example"
QUERY_CASE = "scratch/2nodes_single-tp"
QUERY_TIMEOUT_SECONDS = 30
CATALOG_CACHE_PATH = Path(".openusim/project/parameter-catalog.json")
PROJECT_TRACE_PARSER_PATH = "scratch/ns-3-ub-tools/trace_analysis/parse_trace.py"

TYPE_ID_LINE_PATTERN = re.compile(r"^\s*(?P<type_id>ns3::(?:Ub\w+|TpConnectionManager))\s*$", re.MULTILINE)
ATTRIBUTE_BLOCK_PATTERN = re.compile(
    r"Attribute:\s*(?P<name>[^\n]+)\n"
    r"Description:\s*(?P<description>[^\n]*)\n"
    r"DataType:\s*(?P<value_type>[^\n]+)\n"
    r"Default:\s*(?P<default_value>[^\n]*)",
    re.MULTILINE,
)
GLOBAL_ENTRY_PATTERN = re.compile(
    r"Global:\s*(?P<name>UB_[^\n]+)\n"
    r"Description:\s*(?P<description>[^\n]*)\n"
    r"DataType:\s*(?P<value_type>[^\n]+)\n"
    r"Default:\s*(?P<default_value>[^\n]*)",
    re.MULTILINE,
)


def _normalize_whitespace(text: str) -> str:
    return " ".join((text or "").split())


def _query_command(flag: str) -> str:
    return f"{QUERY_PROGRAM} {QUERY_CASE} {flag}"


def _ensure_query_prerequisites(current_repo_root: Path) -> Path:
    launcher_path = current_repo_root / "ns3"
    case_path = current_repo_root / QUERY_CASE
    if not launcher_path.is_file():
        raise FileNotFoundError(f"Missing ns-3 launcher: {launcher_path}")
    if not case_path.is_dir():
        raise FileNotFoundError(f"Missing query case directory: {case_path}")
    return launcher_path


def _run_query(flag: str) -> str:
    """Run an ns-3 metadata query with the Python executable that launched this tool."""
    current_repo_root = repo_root()
    launcher_path = _ensure_query_prerequisites(current_repo_root)
    result = subprocess.run(
        [sys.executable, str(launcher_path), "run", _query_command(flag)],
        cwd=current_repo_root,
        capture_output=True,
        text=True,
        check=False,
        timeout=QUERY_TIMEOUT_SECONDS,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown error"
        raise RuntimeError(f"ns-3 query failed for {_query_command(flag)}: {detail}")
    return result.stdout + result.stderr


def _runtime_type_ids() -> list[str]:
    output = _run_query("--PrintTypeIds")
    return sorted(set(TYPE_ID_LINE_PATTERN.findall(output)))


def _runtime_attribute_entries() -> list[dict]:
    entries = []
    for type_id in _runtime_type_ids():
        output = _run_query(f"--ClassName={type_id}")
        component_name = type_id.split("::")[-1]
        for match in ATTRIBUTE_BLOCK_PATTERN.finditer(output):
            entries.append(
                {
                    "parameter_key": f"{type_id}::{match.group('name')}",
                    "kind": "AddAttribute",
                    "description": _normalize_whitespace(match.group("description")),
                    "value_type": _normalize_whitespace(match.group("value_type")),
                    "default_value": _normalize_whitespace(match.group("default_value")),
                    "module": component_name,
                    "category": "general",
                    "safety_sensitivity": "medium",
                    "tuning_stage": "guided",
                }
            )
    return entries


def _runtime_global_entries() -> list[dict]:
    output = _run_query("--PrintUbGlobals")
    entries = []
    for match in GLOBAL_ENTRY_PATTERN.finditer(output):
        entries.append(
            {
                "parameter_key": match.group("name"),
                "kind": "GlobalValue",
                "description": _normalize_whitespace(match.group("description")),
                "value_type": _normalize_whitespace(match.group("value_type")),
                "default_value": _normalize_whitespace(match.group("default_value")),
                "module": "GlobalValue",
                "category": "general",
                "safety_sensitivity": "medium",
                "tuning_stage": "guided",
            }
        )
    return entries


def _catalog_cache_path() -> Path:
    return repo_root() / CATALOG_CACHE_PATH


def _read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, data: dict) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def _cache_is_stale(cache_path: Path) -> bool:
    """Return True if the cache is missing or older than the build/ directory."""
    if not cache_path.is_file():
        return True
    try:
        if not _read_json(cache_path).get("entries"):
            return True
    except (OSError, ValueError, TypeError):
        return True
    build_dir = repo_root() / "build"
    if not build_dir.is_dir():
        return True
    return build_dir.stat().st_mtime > cache_path.stat().st_mtime


def load_or_build_parameter_catalog() -> tuple[dict, Path]:
    cache_path = _catalog_cache_path()
    if not _cache_is_stale(cache_path):
        return _read_json(cache_path), cache_path

    entries = _runtime_attribute_entries()
    entries.extend(_runtime_global_entries())
    entries = sorted(entries, key=lambda entry: entry["parameter_key"])
    if not entries:
        raise RuntimeError("runtime parameter catalog query returned no entries")
    catalog = {
        "source_root": str(repo_root() / QUERY_PROGRAM),
        "extractor_version": "runtime-query-v1",
        "entry_count": len(entries),
        "entries": entries,
    }
    return catalog, _write_json(cache_path, catalog)


_OBSERVABILITY_PRESETS = {
    "minimal": {
        "UB_TRACE_ENABLE": "true",
        "UB_TASK_TRACE_ENABLE": "true",
        "UB_PACKET_TRACE_ENABLE": "false",
        "UB_PORT_TRACE_ENABLE": "false",
        "UB_QUEUE_TRACE_ENABLE": "false",
        "UB_RECORD_PKT_TRACE": "false",
        "UB_PARSE_TRACE_ENABLE": "true",
    },
    "balanced": {
        "UB_TRACE_ENABLE": "true",
        "UB_TASK_TRACE_ENABLE": "true",
        "UB_PACKET_TRACE_ENABLE": "true",
        "UB_PORT_TRACE_ENABLE": "true",
        "UB_QUEUE_TRACE_ENABLE": "true",
        "UB_RECORD_PKT_TRACE": "false",
        "UB_PARSE_TRACE_ENABLE": "true",
    },
    "detailed": {
        "UB_TRACE_ENABLE": "true",
        "UB_TASK_TRACE_ENABLE": "true",
        "UB_PACKET_TRACE_ENABLE": "true",
        "UB_PORT_TRACE_ENABLE": "true",
        "UB_QUEUE_TRACE_ENABLE": "true",
        "UB_RECORD_PKT_TRACE": "true",
        "UB_PARSE_TRACE_ENABLE": "true",
    },
}

_FALLBACK_UB_GLOBAL_KEYS = {
    "UB_FAULT_ENABLE",
    "UB_PRIORITY_NUM",
    "UB_VL_NUM",
    "UB_CC_ALGO",
    "UB_CC_ENABLED",
    "UB_TRACE_ENABLE",
    "UB_TASK_TRACE_ENABLE",
    "UB_PACKET_TRACE_ENABLE",
    "UB_PORT_TRACE_ENABLE",
    "UB_QUEUE_TRACE_ENABLE",
    "UB_PARSE_TRACE_ENABLE",
    "UB_RECORD_PKT_TRACE",
    "UB_PYTHON_SCRIPT_PATH",
}


def observability_preset(tier: str) -> dict:
    """Return observability_overrides dict for a named tier.

    Valid tiers: "minimal", "balanced", "detailed".
    Raises ValueError for unknown tiers.
    """
    if tier not in _OBSERVABILITY_PRESETS:
        raise ValueError(f"Unknown observability tier {tier!r}, expected one of {sorted(_OBSERVABILITY_PRESETS)}")
    return dict(_OBSERVABILITY_PRESETS[tier])


def _normalize_override_key(parameter_key: str) -> str:
    if parameter_key.startswith("default "):
        return parameter_key[len("default ") :]
    if parameter_key.startswith("global "):
        return parameter_key[len("global ") :]
    return parameter_key


def _infer_kind(parameter_key: str) -> str:
    return "default" if parameter_key.startswith("ns3::") else "global"


def _render_line(kind: str, key: str, value: str) -> str:
    return f'{kind} {key} "{value}"'


def _merge_overrides(*override_groups: dict | None) -> dict:
    merged = {}
    for group in override_groups:
        for key, value in (group or {}).items():
            merged[_normalize_override_key(key)] = value
    return merged


def validate_overrides_against_catalog(
    catalog: dict,
    explicit_overrides: dict | None = None,
    observability_overrides: dict | None = None,
) -> dict:
    """Validate overrides against the current runtime-catalog surface.

    The catalog is authoritative for runtime-enumerated `ns3::...` attributes.
    For UB globals, prefer runtime enumeration when available; otherwise allow only
    the documented minimal UB-global fallback set needed by repo tooling.
    """
    merged_overrides = _merge_overrides(observability_overrides, explicit_overrides)
    catalog_entry_by_key = {entry["parameter_key"]: entry for entry in catalog.get("entries", [])}
    runtime_global_keys = {
        entry["parameter_key"]
        for entry in catalog.get("entries", [])
        if entry.get("kind") == "GlobalValue"
    }
    has_runtime_global_catalog = bool(runtime_global_keys)

    accepted_keys = []
    unknown_keys = []
    for key in sorted(merged_overrides):
        if key in catalog_entry_by_key:
            accepted_keys.append(key)
            continue

        if _infer_kind(key) == "default":
            unknown_keys.append(key)
            continue

        if has_runtime_global_catalog:
            if key in runtime_global_keys:
                accepted_keys.append(key)
            else:
                unknown_keys.append(key)
            continue

        if key in _FALLBACK_UB_GLOBAL_KEYS:
            accepted_keys.append(key)
        else:
            unknown_keys.append(key)

    if unknown_keys:
        raise ValueError(
            "Unsupported network override keys for the current project surface: "
            + ", ".join(sorted(unknown_keys))
        )

    return {
        "accepted_keys": accepted_keys,
        "unknown_keys": unknown_keys,
        "validation_mode": (
            "runtime-addattribute-and-global-catalog"
            if has_runtime_global_catalog
            else "runtime-addattribute-catalog-with-documented-ub-global-fallback"
        ),
    }


def write_network_attributes(case_dir: Path, explicit_overrides=None, observability_overrides=None) -> dict:
    case_dir = Path(case_dir)
    case_dir.mkdir(parents=True, exist_ok=True)

    catalog, catalog_path = load_or_build_parameter_catalog()
    validation_result = validate_overrides_against_catalog(
        catalog,
        explicit_overrides=explicit_overrides,
        observability_overrides=observability_overrides,
    )
    # Later groups override earlier ones: explicit user overrides take precedence
    merged_overrides = _merge_overrides(observability_overrides, explicit_overrides)
    resolved_values = {entry["parameter_key"]: entry["default_value"] for entry in catalog["entries"]}
    resolved_values.update(merged_overrides)
    resolved_values["UB_PYTHON_SCRIPT_PATH"] = PROJECT_TRACE_PARSER_PATH

    catalog_entry_by_key = {entry["parameter_key"]: entry for entry in catalog["entries"]}
    default_keys = sorted(
        key for key, entry in catalog_entry_by_key.items() if entry["kind"] == "AddAttribute"
    )
    global_keys = sorted(
        key for key, entry in catalog_entry_by_key.items() if entry["kind"] == "GlobalValue"
    )
    extra_default_keys = sorted(
        key for key in resolved_values if key not in catalog_entry_by_key and _infer_kind(key) == "default"
    )
    extra_global_keys = sorted(
        key for key in resolved_values if key not in catalog_entry_by_key and _infer_kind(key) == "global"
    )

    lines = [_render_line("default", key, resolved_values[key]) for key in default_keys + extra_default_keys]
    if global_keys or extra_global_keys:
        lines.append("")
        lines.extend(_render_line("global", key, resolved_values[key]) for key in global_keys + extra_global_keys)

    output_path = case_dir / "network_attribute.txt"
    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return {
        "path": str(output_path),
        "resolution_mode": "full-catalog-snapshot",
        "parameter_catalog_source": str(catalog_path),
        "required_project_pins": {"UB_PYTHON_SCRIPT_PATH": PROJECT_TRACE_PARSER_PATH},
        "resolved_entry_count": len([line for line in lines if line.strip()]),
        "applied_overrides": merged_overrides,
        "validation_mode": validation_result["validation_mode"],
    }
