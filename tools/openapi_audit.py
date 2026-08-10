#!/usr/bin/env python3
"""Compare a supplied Venice OpenAPI document with the checked-in manifest.

This is maintainer tooling, not a build dependency. It deliberately reads a
local file and never downloads the live specification itself.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


HTTP_METHODS = {"get", "put", "post", "delete", "options", "head", "patch", "trace"}


class AuditInputError(RuntimeError):
    """The manifest or supplied OpenAPI document cannot be audited safely."""


def load_spec(path: Path) -> tuple[dict[str, Any], str]:
    try:
        from ruamel.yaml import YAML
    except ImportError as exc:
        raise AuditInputError(
            "ruamel.yaml is required; install tools/openapi_audit_requirements.txt"
        ) from exc

    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise AuditInputError(f"{path}: specification is not UTF-8") from exc

    yaml = YAML(typ="safe", pure=True)
    yaml.version = (1, 2)
    yaml.allow_duplicate_keys = False
    try:
        document = yaml.load(text)
    except Exception as exc:  # ruamel exposes several parser exception types
        raise AuditInputError(f"{path}: invalid YAML/JSON: {exc}") from exc
    if not isinstance(document, dict):
        raise AuditInputError(f"{path}: specification root must be an object")
    return document, hashlib.sha256(raw).hexdigest()


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AuditInputError(f"{path}: invalid manifest: {exc}") from exc
    if not isinstance(value, dict):
        raise AuditInputError(f"{path}: manifest root must be an object")
    return value


def _resolve_local_ref(
    node: Any, document: dict[str, Any], trail: tuple[str, ...] = ()
) -> Any:
    if not isinstance(node, dict) or "$ref" not in node:
        return node
    ref = node["$ref"]
    if not isinstance(ref, str) or not ref.startswith("#/"):
        raise AuditInputError(f"unsupported external or malformed reference: {ref!r}")
    if ref in trail:
        raise AuditInputError(f"cyclic local reference: {' -> '.join((*trail, ref))}")

    current: Any = document
    for token in ref[2:].split("/"):
        token = token.replace("~1", "/").replace("~0", "~")
        if not isinstance(current, dict) or token not in current:
            raise AuditInputError(f"unresolved local reference: {ref}")
        current = current[token]
    return _resolve_local_ref(current, document, (*trail, ref))


def _security_modes(value: Any) -> list[list[str]]:
    # No OpenAPI security declaration means anonymous access. An explicit empty
    # security array has the same effective meaning; [{}] is the alternative
    # spelling used when anonymous is one option among authenticated modes.
    if value is None or value == []:
        return [[]]
    if not isinstance(value, list):
        raise AuditInputError("security must be an array")
    modes: list[list[str]] = []
    for requirement in value:
        if not isinstance(requirement, dict):
            raise AuditInputError("each security alternative must be an object")
        modes.append(sorted(str(name) for name in requirement))
    return sorted(modes)


def spec_contract(document: dict[str, Any], digest: str) -> dict[str, Any]:
    info = document.get("info")
    paths = document.get("paths")
    if not isinstance(info, dict) or not isinstance(info.get("version"), str):
        raise AuditInputError("specification info.version must be a string")
    if not isinstance(paths, dict):
        raise AuditInputError("specification paths must be an object")

    root_security = document.get("security")
    operations: dict[tuple[str, str], dict[str, Any]] = {}
    for path, path_item in paths.items():
        if not isinstance(path, str) or not isinstance(path_item, dict):
            raise AuditInputError("every path item must be an object keyed by a string path")
        path_item = _resolve_local_ref(path_item, document)
        if not isinstance(path_item, dict):
            raise AuditInputError(f"{path}: path item must resolve to an object")
        for method, operation in path_item.items():
            if str(method).lower() not in HTTP_METHODS:
                continue
            if not isinstance(operation, dict):
                raise AuditInputError(
                    f"{str(method).upper()} {path}: operation must be an object"
                )

            method_upper = str(method).upper()
            key = (method_upper, path)
            if key in operations:
                raise AuditInputError(f"duplicate operation {method_upper} {path}")

            request_body = operation.get("requestBody")
            if request_body is not None:
                request_body = _resolve_local_ref(request_body, document)
                if not isinstance(request_body, dict):
                    raise AuditInputError(
                        f"{method_upper} {path}: requestBody must resolve to an object"
                    )
            request_content = (request_body or {}).get("content", {})
            if not isinstance(request_content, dict):
                raise AuditInputError(f"{method_upper} {path}: requestBody content must be an object")
            request_media = sorted(request_content.keys())

            responses = operation.get("responses")
            if not isinstance(responses, dict):
                raise AuditInputError(f"{method_upper} {path}: responses must be an object")
            response_media: dict[str, list[str]] = {}
            for status, response in responses.items():
                response = _resolve_local_ref(response, document)
                if not isinstance(response, dict):
                    raise AuditInputError(
                        f"{method_upper} {path}: response {status} must resolve to an object"
                    )
                content = response.get("content", {})
                if not isinstance(content, dict):
                    raise AuditInputError(
                        f"{method_upper} {path}: response {status} content must be an object"
                    )
                response_media[str(status)] = sorted(content.keys())

            security = operation["security"] if "security" in operation else root_security
            operations[key] = {
                "security": _security_modes(security),
                "request_media": request_media,
                "response_media": dict(sorted(response_media.items())),
            }

    return {"version": info["version"], "sha256": digest, "operations": operations}


def manifest_contract(manifest: dict[str, Any]) -> dict[str, Any]:
    source = manifest.get("source")
    rows = manifest.get("operations")
    if not isinstance(source, dict) or not isinstance(rows, list):
        raise AuditInputError("manifest requires source object and operations array")
    operations: dict[tuple[str, str], dict[str, Any]] = {}
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise AuditInputError(f"manifest operations[{index}] must be an object")
        try:
            key = (row["method"], row["path"])
            contract = {name: row[name] for name in ("security", "request_media", "response_media")}
        except KeyError as exc:
            raise AuditInputError(f"manifest operations[{index}] missing {exc.args[0]}") from exc
        if not all(isinstance(part, str) for part in key):
            raise AuditInputError(f"manifest operations[{index}] method/path must be strings")
        if key in operations:
            raise AuditInputError(f"manifest contains duplicate operation {key[0]} {key[1]}")
        operations[key] = contract
    return {
        "version": source.get("version"),
        "sha256": source.get("sha256"),
        "operations": operations,
    }


def compare_contracts(expected: dict[str, Any], observed: dict[str, Any]) -> list[str]:
    changes: list[str] = []
    for field in ("version", "sha256"):
        if expected[field] != observed[field]:
            changes.append(f"source.{field}: {expected[field]!r} -> {observed[field]!r}")

    expected_ops = expected["operations"]
    observed_ops = observed["operations"]
    operation_order = lambda item: (item[1], item[0])
    for method, path in sorted(
        observed_ops.keys() - expected_ops.keys(), key=operation_order
    ):
        changes.append(f"added operation: {method} {path}")
    for method, path in sorted(
        expected_ops.keys() - observed_ops.keys(), key=operation_order
    ):
        changes.append(f"removed operation: {method} {path}")
    for key in sorted(expected_ops.keys() & observed_ops.keys(), key=operation_order):
        method, path = key
        for field in ("security", "request_media", "response_media"):
            if expected_ops[key][field] != observed_ops[key][field]:
                changes.append(
                    f"changed {method} {path} {field}: "
                    f"{json.dumps(expected_ops[key][field], sort_keys=True)} -> "
                    f"{json.dumps(observed_ops[key][field], sort_keys=True)}"
                )
    return changes


def parse_args(argv: list[str]) -> argparse.Namespace:
    default_manifest = (
        Path(__file__).resolve().parents[1] / "cmake" / "openapi_manifest.json"
    )
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spec", type=Path, help="local OpenAPI YAML or JSON file")
    parser.add_argument("--manifest", type=Path, default=default_manifest)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        manifest = manifest_contract(load_manifest(args.manifest))
        document, digest = load_spec(args.spec)
        observed = spec_contract(document, digest)
        changes = compare_contracts(manifest, observed)
    except (AuditInputError, OSError) as exc:
        print(f"openapi-audit: input error: {exc}", file=sys.stderr)
        return 2

    if changes:
        print(f"OpenAPI drift detected ({len(changes)} change(s)):")
        for change in changes:
            print(f"  - {change}")
        return 1
    print(
        f"OpenAPI manifest matches {observed['version']} "
        f"({len(observed['operations'])} operations, sha256 {observed['sha256']})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
