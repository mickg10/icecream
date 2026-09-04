"""Small, dependency-free validator for the committed farm JSON schemas.

The integration hub contract requires Python 3.10+, not a separately managed
Python environment.  This module implements only the Draft 2020-12 keywords
used by ``schemas/farm-v1.json`` and ``schemas/scenario-v1.json``.  Known
annotation keywords are ignored; every unknown assertion keyword is refused
so a schema upgrade cannot silently weaken validation.
"""

from __future__ import annotations

import json
import math
import re
from pathlib import Path
from typing import Any, NoReturn


class ValidationError(ValueError):
    """A JSON document or its schema contract is invalid."""


_SCHEMA_KEYWORDS = frozenset(
    {
        "$comment",
        "$defs",
        "$id",
        "$ref",
        "$schema",
        "additionalProperties",
        "allOf",
        "const",
        "default",
        "deprecated",
        "description",
        "enum",
        "examples",
        "exclusiveMinimum",
        "if",
        "items",
        "maxItems",
        "maxLength",
        "maximum",
        "minItems",
        "minLength",
        "minProperties",
        "minimum",
        "pattern",
        "prefixItems",
        "properties",
        "propertyNames",
        "readOnly",
        "required",
        "then",
        "title",
        "type",
        "uniqueItems",
        "writeOnly",
    }
)


def _fail(location: str, message: str) -> NoReturn:
    raise ValidationError(f"{location}: {message}")


def _pairs_no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            _fail("$", f"duplicate key {key!r}")
        value[key] = item
    return value


def _reject_constant(value: str) -> NoReturn:
    _fail("$", f"non-finite number {value!r} is forbidden")


def _parse_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        _reject_constant(value)
    return parsed


def _reject_surrogates(value: Any, location: str = "$") -> None:
    """Reject strings Python can represent but UTF-8 and remote argv cannot."""

    if isinstance(value, str):
        if any(0xD800 <= ord(character) <= 0xDFFF for character in value):
            _fail(location, "Unicode surrogate code points are forbidden")
        return
    if isinstance(value, list):
        for index, item in enumerate(value):
            _reject_surrogates(item, f"{location}[{index}]")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            _reject_surrogates(key, f"{location}.<key>")
            _reject_surrogates(item, f"{location}.{key}")


def load_json(path: Path) -> Any:
    """Read strict UTF-8 JSON, rejecting duplicate keys and NaN/Infinity."""

    try:
        raw = path.read_text(encoding="utf-8")
    except UnicodeError as exc:
        raise ValidationError(f"{path}: invalid UTF-8: {exc}") from exc
    except OSError as exc:
        raise ValidationError(f"{path}: cannot read: {exc}") from exc
    try:
        value = json.loads(
            raw,
            object_pairs_hook=_pairs_no_duplicates,
            parse_constant=_reject_constant,
            parse_float=_parse_float,
        )
    except (json.JSONDecodeError, UnicodeError) as exc:
        raise ValidationError(f"{path}: invalid JSON: {exc}") from exc
    _reject_surrogates(value)
    return value


def canonical_bytes(value: Any) -> bytes:
    """Canonical encoding used for all plan and topology digests."""

    return (
        json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
            allow_nan=False,
        )
        + "\n"
    ).encode()


def _resolve_ref(root: dict[str, Any], ref: str) -> dict[str, Any]:
    if not ref.startswith("#/"):
        _fail("$schema", f"non-local reference {ref!r} is unsupported")
    value: Any = root
    for encoded in ref[2:].split("/"):
        key = encoded.replace("~1", "/").replace("~0", "~")
        if not isinstance(value, dict) or key not in value:
            _fail("$schema", f"reference {ref!r} does not exist")
        value = value[key]
    if not isinstance(value, dict):
        _fail("$schema", f"reference {ref!r} is not an object schema")
    return value


def _audit_schema(schema: dict[str, Any], location: str = "$schema") -> None:
    for keyword in schema:
        if keyword not in _SCHEMA_KEYWORDS:
            _fail(location, f"unsupported schema keyword {keyword!r}")

    for keyword in ("$defs", "properties"):
        children = schema.get(keyword, {})
        if isinstance(children, dict):
            for name, child in children.items():
                if isinstance(child, dict):
                    _audit_schema(child, f"{location}.{keyword}.{name}")

    for keyword in ("additionalProperties", "if", "items", "propertyNames"):
        child = schema.get(keyword)
        if isinstance(child, dict):
            _audit_schema(child, f"{location}.{keyword}")

    then = schema.get("then")
    if isinstance(then, dict):
        _audit_schema(then, f"{location}.then")

    for keyword in ("allOf", "prefixItems"):
        children = schema.get(keyword, [])
        if isinstance(children, list):
            for index, child in enumerate(children):
                if isinstance(child, dict):
                    _audit_schema(child, f"{location}.{keyword}[{index}]")


def _is_type(value: Any, name: str) -> bool:
    if name == "object":
        return isinstance(value, dict)
    if name == "array":
        return isinstance(value, list)
    if name == "string":
        return isinstance(value, str)
    if name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)
    if name == "boolean":
        return isinstance(value, bool)
    if name == "null":
        return value is None
    _fail("$schema", f"unsupported type {name!r}")


def _matches(value: Any, schema: dict[str, Any], root: dict[str, Any]) -> bool:
    try:
        _validate(value, schema, root, "$")
    except ValidationError:
        return False
    return True


def _validate(value: Any, schema: dict[str, Any], root: dict[str, Any], location: str) -> None:
    if "$ref" in schema:
        _validate(value, _resolve_ref(root, schema["$ref"]), root, location)

    if "const" in schema and value != schema["const"]:
        _fail(location, f"must equal {schema['const']!r}")
    if "enum" in schema and value not in schema["enum"]:
        _fail(location, f"must be one of {schema['enum']!r}")

    if "type" in schema:
        names = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_is_type(value, name) for name in names):
            _fail(location, f"must have type {' or '.join(names)}")

    for subschema in schema.get("allOf", []):
        _validate(value, subschema, root, location)
    if "if" in schema and _matches(value, schema["if"], root) and "then" in schema:
        _validate(value, schema["then"], root, location)

    if isinstance(value, dict):
        required = schema.get("required", [])
        for key in required:
            if key not in value:
                _fail(location, f"missing required property {key!r}")
        if len(value) < schema.get("minProperties", 0):
            _fail(location, f"must have at least {schema['minProperties']} properties")
        if "propertyNames" in schema:
            for key in value:
                _validate(key, schema["propertyNames"], root, f"{location}.<key>")
        properties = schema.get("properties", {})
        additional = schema.get("additionalProperties", True)
        for key, item in value.items():
            child = f"{location}.{key}"
            if key in properties:
                _validate(item, properties[key], root, child)
            elif additional is False:
                _fail(child, "additional property is forbidden")
            elif isinstance(additional, dict):
                _validate(item, additional, root, child)

    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            _fail(location, f"must have at least {schema['minItems']} items")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            _fail(location, f"must have at most {schema['maxItems']} items")
        if schema.get("uniqueItems"):
            seen: set[bytes] = set()
            for item in value:
                encoded = canonical_bytes(item)
                if encoded in seen:
                    _fail(location, "items must be unique")
                seen.add(encoded)
        prefix = schema.get("prefixItems", [])
        for index, item in enumerate(value[: len(prefix)]):
            _validate(item, prefix[index], root, f"{location}[{index}]")
        if len(value) > len(prefix):
            items = schema.get("items", {})
            if items is False:
                _fail(location, f"must have at most {len(prefix)} items")
            if isinstance(items, dict):
                for index, item in enumerate(value[len(prefix) :], start=len(prefix)):
                    _validate(item, items, root, f"{location}[{index}]")

    if isinstance(value, str):
        if len(value) < schema.get("minLength", 0):
            _fail(location, f"must have length at least {schema['minLength']}")
        if "maxLength" in schema and len(value) > schema["maxLength"]:
            _fail(location, f"must have length at most {schema['maxLength']}")
        if "pattern" in schema and re.search(schema["pattern"], value) is None:
            _fail(location, f"does not match {schema['pattern']!r}")

    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            _fail(location, f"must be >= {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            _fail(location, f"must be <= {schema['maximum']}")
        if "exclusiveMinimum" in schema and value <= schema["exclusiveMinimum"]:
            _fail(location, f"must be > {schema['exclusiveMinimum']}")


def validate(value: Any, schema_path: Path) -> None:
    schema = load_json(schema_path)
    if not isinstance(schema, dict):
        _fail("$schema", "root must be an object")
    _audit_schema(schema)
    _validate(value, schema, schema, "$")
