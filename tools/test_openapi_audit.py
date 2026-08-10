#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from openapi_audit import AuditInputError, compare_contracts, spec_contract


def contract(text: str):
    from ruamel.yaml import YAML

    yaml = YAML(typ="safe", pure=True)
    yaml.version = (1, 2)
    yaml.allow_duplicate_keys = False
    document = yaml.load(text)
    return document, spec_contract(document, hashlib.sha256(text.encode()).hexdigest())


BASE = """\
openapi: 3.0.0
info: {title: fixture, version: '1'}
security:
  - BearerAuth: []
paths:
  /without-operation-id:
    get:
      responses:
        '200':
          content:
            application/json: {}
"""


class OpenApiAuditTests(unittest.TestCase):
    # Failure matrix first.
    def test_added_and_removed_operations_are_reported(self):
        _, old = contract(BASE)
        _, new = contract(BASE.replace("/without-operation-id", "/new"))
        changes = compare_contracts(old, new)
        self.assertTrue(any("added operation: GET /new" in item for item in changes))
        self.assertTrue(
            any("removed operation: GET /without-operation-id" in item for item in changes)
        )

    def test_security_and_media_drift_are_reported(self):
        _, old = contract(BASE)
        changed = BASE.replace("- BearerAuth: []", "- siwx: []").replace(
            "application/json: {}", "text/plain: {}"
        )
        _, new = contract(changed)
        changes = compare_contracts(old, new)
        self.assertTrue(any("security" in item for item in changes))
        self.assertTrue(any("response_media" in item for item in changes))

    def test_request_media_drift_is_reported(self):
        _, old = contract(BASE)
        changed = BASE.replace(
            "get:\n      responses:",
            "get:\n      requestBody:\n        content:\n          application/json: {}\n      responses:",
        )
        _, new = contract(changed)
        changes = compare_contracts(old, new)
        self.assertTrue(any("request_media" in item for item in changes))

    def test_anonymous_override_is_distinct_from_inherited_bearer(self):
        _, inherited = contract(BASE)
        _, anonymous = contract(
            BASE.replace(
                "get:\n      responses:", "get:\n      security: []\n      responses:"
            )
        )
        changes = compare_contracts(inherited, anonymous)
        self.assertTrue(any("security" in item for item in changes))

    def test_external_reference_fails_instead_of_underreporting(self):
        text = BASE.replace(
            "'200':\n          content:\n            application/json: {}",
            "'200': {$ref: 'other.yaml#/components/responses/Ok'}",
        )
        with self.assertRaisesRegex(AuditInputError, "external"):
            contract(text)

    def test_unresolved_local_reference_fails(self):
        text = BASE.replace(
            "'200':\n          content:\n            application/json: {}",
            "'200': {$ref: '#/components/responses/Missing'}",
        )
        with self.assertRaisesRegex(AuditInputError, "unresolved"):
            contract(text)

    def test_local_response_reference_is_resolved(self):
        text = BASE.replace(
            "'200':\n          content:\n            application/json: {}",
            "'200': {$ref: '#/components/responses/Ok'}",
        ) + "components:\n  responses:\n    Ok:\n      content:\n        text/plain: {}\n"
        _, observed = contract(text)
        operation = observed["operations"][("GET", "/without-operation-id")]
        self.assertEqual(operation["response_media"], {"200": ["text/plain"]})

    def test_yaml_12_preserves_aspect_ratio_scalar(self):
        document, _ = contract(BASE + "x-aspect-ratio: 16:9\n")
        self.assertEqual(document["x-aspect-ratio"], "16:9")

    # Happy path last: operationId is intentionally absent and irrelevant.
    def test_z_unchanged_contract_matches_without_operation_id(self):
        _, expected = contract(BASE)
        _, observed = contract(BASE)
        self.assertEqual(compare_contracts(expected, observed), [])


if __name__ == "__main__":
    unittest.main()
