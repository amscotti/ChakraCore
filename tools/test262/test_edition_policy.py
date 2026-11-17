#-------------------------------------------------------------------------------------------------------
# Copyright (C) Microsoft. All rights reserved.
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""
Unit tests for tools/test262/edition-policy.json.
Validates structural integrity, feature classifications, and synchronization
with default-on engine configuration flags in lib/Common/ConfigFlagsList.h.
"""

import json
from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
POLICY_PATH = HERE / "edition-policy.json"
CONFIG_FLAGS_PATH = ROOT / "lib/Common/ConfigFlagsList.h"

sys.path.insert(0, str(HERE))
import analyze


class TestEditionPolicy(unittest.TestCase):
    def setUp(self):
        self.assertTrue(POLICY_PATH.exists(), f"Policy file missing: {POLICY_PATH}")
        with open(POLICY_PATH, "r", encoding="utf-8") as f:
            self.policy = json.load(f)

    def test_top_level_keys(self):
        expected_keys = {
            "categories",
            "corpus_revision",
            "finished_proposals_provenance",
            "disabled_by_default_flags",
            "features",
            "host_optional",
            "limits",
            "policy",
            "rule",
            "selected_patterns",
        }
        self.assertEqual(set(self.policy.keys()), expected_keys)

    def test_categories(self):
        categories = self.policy.get("categories", {})
        self.assertIn("required-ES2025", categories)
        self.assertIn("post-ES2025-proposal", categories)
        self.assertIn("host-optional", categories)

    def test_default_on_features_not_in_disabled_flags(self):
        disabled_flags = self.policy.get("disabled_by_default_flags", {})
        # Features that are default-on in ChakraCore must not be in disabled_by_default_flags
        default_on_flags = [
            "ES6RegExPrototypeProperties",
            "ES6RegExSymbols",
            "ESSharedArrayBuffer",
            "ESBigInt",
        ]
        for flag in default_on_flags:
            self.assertNotIn(
                flag,
                disabled_flags,
                f"Flag {flag} is default-on in ChakraCore and must not be in disabled_by_default_flags",
            )

    def test_synchronized_features_categorized_required(self):
        features = self.policy.get("features", {})
        expected_required = [
            "SharedArrayBuffer",
            "Atomics",
            "Atomics.waitAsync",
            "Symbol.match",
            "Symbol.matchAll",
            "Symbol.replace",
            "Symbol.search",
            "Symbol.split",
        ]
        for feat in expected_required:
            self.assertIn(feat, features, f"Expected feature {feat} to be in edition-policy.json")
            self.assertEqual(
                features[feat].get("category"),
                "required-ES2025",
                f"Feature {feat} must be categorized as required-ES2025",
            )

    def test_all_features_well_formed(self):
        categories = set(self.policy.get("categories", {}).keys())
        features = self.policy.get("features", {})
        self.assertGreater(len(features), 100)
        for feat_name, feat_data in features.items():
            self.assertIn("category", feat_data, f"Feature {feat_name} missing 'category'")
            self.assertIn(
                feat_data["category"],
                categories,
                f"Feature {feat_name} has invalid category {feat_data['category']}",
            )
            self.assertIn("rationale", feat_data, f"Feature {feat_name} missing 'rationale'")
            self.assertIn("source", feat_data, f"Feature {feat_name} missing 'source'")

    def test_load_policy_integration(self):
        policy, category, flag_tags = analyze.load_policy(POLICY_PATH)
        self.assertIsInstance(category, dict)
        self.assertIsInstance(flag_tags, dict)
        # All default-on feature tags must NOT be present in flag_tags
        unwanted_flag_tags = [
            "SharedArrayBuffer",
            "Atomics",
            "Atomics.waitAsync",
            "Symbol.match",
            "Symbol.matchAll",
            "Symbol.replace",
            "Symbol.search",
            "Symbol.split",
            "BigInt",
        ]
        for tag in unwanted_flag_tags:
            self.assertNotIn(
                tag,
                flag_tags,
                f"Tag {tag} should not be mapped in flag_tags since its flag is default-on",
            )

    def test_failure_bucket_not_disabled_flag(self):
        _, category, flag_tags = analyze.load_policy(POLICY_PATH)
        # Scenario with SAB failure should not be bucketed as 'disabled-flag'
        sab_record = {
            "file": "test/built-ins/SharedArrayBuffer/basic.js",
            "scenario": "default",
            "attrs": {"features": ["SharedArrayBuffer"]},
            "result": {"pass": False, "message": "ReferenceError: SharedArrayBuffer is not defined"},
            "rawResult": {"error": {"name": "ReferenceError", "message": "SharedArrayBuffer is not defined"}},
        }
        bucket = analyze.failure_bucket(sab_record, category, flag_tags)
        self.assertNotEqual(
            bucket,
            "disabled-flag",
            "SharedArrayBuffer failure should not bucket as disabled-flag when default-on",
        )
        self.assertEqual(bucket, "required-feature-missing")


if __name__ == "__main__":
    unittest.main()
