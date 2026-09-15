#!/usr/bin/env python3
"""Regression checks for secret rejection and index-vs-working-tree behavior."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from check_public_tree import inspect, tracked_entries


class PublicTreeTest(unittest.TestCase):
    def test_rejects_sensitive_files_and_binary(self):
        for name in ('User/OtaSecrets.h', '.env', '.codex/auth.json', 'firmware.bin', 'Objects/a.o'):
            with self.subTest(name=name):
                self.assertTrue(inspect(name, b'example'))
        self.assertTrue(inspect('readme.md', b'\x00binary'))

    def test_secret_rules(self):
        for value in ('sk-' + 'A' * 40, 'ghp_' + 'B' * 40,
                      '-----BEGIN ' + 'PRIVATE KEY-----',
                      'https://example.invalid/file?' + 'Signature=example',
                      '#define MQTT_PASSWORD "test-secret"',
                      '#define OTA_FIRMWARE_HMAC_KEY_HEX "' + '1' * 64 + '"'):
            with self.subTest():
                self.assertTrue(inspect('example.h', value.encode()))

    def test_template_and_fixed_test_vector_are_allowed(self):
        value = '#define MQTT_PASSWORD "replace-with-device-password"\n'
        self.assertEqual(inspect('User/OtaSecrets.example.h', value.encode()), [])
        self.assertEqual(inspect('test.py', b'key = bytes(range(32))'), [])

    def test_reads_secret_from_index_even_after_working_file_is_cleaned(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'repo-\u6d4b\u8bd5'
            root.mkdir()
            subprocess.run(['git', 'init', '-q'], cwd=root, check=True)
            path = root / 'example.h'
            path.write_text('#define MQTT_PASSWORD "test-secret"\n', encoding='utf-8')
            subprocess.run(['git', 'add', 'example.h'], cwd=root, check=True)
            path.write_text('// cleaned working copy\n', encoding='utf-8')
            staged = list(tracked_entries(root))
            self.assertEqual(len(staged), 1)
            self.assertTrue(inspect(*staged[0]))
            self.assertEqual(inspect('example.h', path.read_bytes()), [])


if __name__ == '__main__':
    unittest.main()
