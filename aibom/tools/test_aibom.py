#!/usr/bin/env python3
"""Synthetic, offline negative tests and private collector checks for revision 3."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
PACKAGE = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('aibom_validator', Path(__file__).with_name('validate_aibom.py'))
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)

class AIBOMTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='athena-aibom-test-')
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.package = self.work / 'package'
        shutil.copytree(PACKAGE, self.package)
        self.ad = self.package / 'aibom'

    def read(self, name):
        return json.loads((self.ad / name).read_text())

    def write(self, name, value, rebind=False):
        (self.ad / name).write_text(json.dumps(value, indent=2) + '\n')
        if rebind:
            bom = self.read('athena-r26.1.cdx.json')
            for ref in bom['externalReferences']:
                if ref['url'] == name:
                    ref['hashes'] = [{'alg': 'SHA-256', 'content': validator.digest(self.ad / name)}]
            self.write('athena-r26.1.cdx.json', bom)

    def rejects(self, phrase):
        with self.assertRaisesRegex(ValueError, phrase):
            validator.validate(self.package, package_checksums=False)

    def test_dangling_flow_endpoint(self):
        graph = self.read('athena-r26.1.graph.json')
        graph['flows'][0]['target'] = 'missing-component'
        self.write('athena-r26.1.graph.json', graph)
        self.rejects('Unresolved flow endpoint')

    def test_tampered_evidence_sidecar(self):
        evidence = self.read('evidence.json')
        evidence['records'][0]['title'] = 'Tampered evidence'
        self.write('evidence.json', evidence)
        self.rejects('sidecar hash mismatch')

    def test_altered_supplied_license(self):
        path = self.ad / 'licensing/snapshot/athena-upload/LICENSE'
        path.write_bytes(path.read_bytes() + b'\nNot the original license\n')
        self.rejects('License snapshot bytes differ')

    def test_swapped_project_notices(self):
        stock = self.ad / 'licensing/snapshot/athena-upload/NOTICE'
        overlay = self.ad / 'licensing/snapshot/athena-consciousness-upload/NOTICE'
        stock.write_bytes(overlay.read_bytes())
        self.rejects('License snapshot bytes differ')

    def test_altered_embedded_license(self):
        bom = self.read('athena-r26.1.cdx.json')
        bom['metadata']['component']['licenses'][0]['license']['text']['content'] += '\nChanged'
        self.write('athena-r26.1.cdx.json', bom)
        self.rejects('Embedded license text differs')

    def test_custom_license_is_not_spdx_list_id(self):
        bom = self.read('athena-r26.1.cdx.json')
        lic = bom['components'][0]['licenses'][0]['license']
        lic.pop('name')
        lic['id'] = 'LicenseRef-Athena-NCARD-2.0'
        self.write('athena-r26.1.cdx.json', bom)
        self.rejects('bom-1.7.schema.json')

    def test_no_mit_alternative_for_new_covered_material(self):
        bom = self.read('athena-r26.1.cdx.json')
        bom['components'][0]['licenses'] = [{'expression': 'MIT OR LicenseRef-Athena-NCARD-2.0', 'acknowledgement': 'declared'}]
        self.write('athena-r26.1.cdx.json', bom)
        self.rejects('Expected scoped named license')

    def test_no_athena_relicensing_of_weights(self):
        bom = self.read('athena-r26.1.cdx.json')
        qwen = next(c for c in bom['components'] if c['bom-ref'] == 'M-qwen')
        qwen['licenses'] = bom['components'][0]['licenses']
        self.write('athena-r26.1.cdx.json', bom)
        self.rejects('Athena license incorrectly applied to excluded component')

    def test_no_invented_adoption(self):
        graph = self.read('athena-r26.1.graph.json')
        graph['header']['licensing']['adoption_commit']['overlay'] = graph['header']['source_revisions']['overlay']['commit']
        self.write('athena-r26.1.graph.json', graph, rebind=True)
        self.rejects('must not invent adoption commits')

    def test_requirement_is_not_implementation(self):
        mapping = self.read('license-mapping.json')
        mapping['obligations'][0]['status'] = 'implemented-and-certified'
        self.write('license-mapping.json', mapping, rebind=True)
        self.rejects('Requirement incorrectly asserted as implemented')

    def test_historical_mit_not_erased(self):
        bom = self.read('athena-r26.1.cdx.json')
        stock = next(c for c in bom['components'] if c['bom-ref'] == 'C-stock')
        stock.pop('evidence')
        self.write('athena-r26.1.cdx.json', bom)
        self.rejects('Historical stock MIT evidence lost')

    def test_projector_hash_binding(self):
        bom = self.read('athena-r26.1.cdx.json')
        projector = next(c for c in bom['components'] if c['bom-ref'] == 'M-projector')
        projector['hashes'][0]['content'] = '0' * 64
        self.write('athena-r26.1.cdx.json', bom)
        self.rejects('Projector expected hash mismatch')

    def test_projector_flow_is_build_only(self):
        graph = self.read('athena-r26.1.graph.json')
        next(f for f in graph['flows'] if f['id'] == 'F50')['phase'] = 'runtime'
        self.write('athena-r26.1.graph.json', graph, rebind=True)
        self.rejects('Projector acquisition flow must remain build-only')

    def test_projector_download_is_not_install_observation(self):
        bom = self.read('athena-r26.1.cdx.json')
        projector = next(c for c in bom['components'] if c['bom-ref'] == 'M-projector')
        next(p for p in projector['properties'] if p['name'] == 'athena:runtime-observed')['value'] = 'true'
        self.write('athena-r26.1.cdx.json', bom)
        self.rejects('Projector download must not become an installation observation')

    def test_license_review_snapshot_not_rewritten(self):
        mapping = self.read('license-mapping.json')
        overlay = next(r for r in mapping['repositories'] if r['id'] == 'overlay')
        overlay['supplied_package_reviewed_commit'] = overlay['runtime_source_commit']
        self.write('license-mapping.json', mapping, rebind=True)
        self.rejects('Supplied licensing-review identity mismatch')

    def test_private_collector_synthetic_fixture(self):
        install = self.work / 'mock-install'
        (install / 'models').mkdir(parents=True)
        model = install / 'models/ggml-small.en.bin'
        model.write_bytes(b'synthetic-model-test')
        (install / 'athena_memory').mkdir()
        (install / 'athena_memory/memory.txt').write_text('PRIVATE_MEMORY_SHOULD_NOT_APPEAR')
        fake = install / 'whisper.cpp/build/bin/whisper-talk-llama'
        fake.parent.mkdir(parents=True)
        marker = self.work / 'binary-was-executed'
        fake.write_text('#!/bin/sh\ntouch "' + str(marker) + '"\n')
        fake.chmod(0o755)
        output = self.work / 'private-output.json'
        env = dict(os.environ, UNRELATED_SECRET_TOKEN='AIBOM_DUMMY_SECRET_CANARY', ATHENA_R26='1')
        cmd = [sys.executable, '-B', str(self.ad / 'tools/collect_deployment.py'), '--athena', str(install), '--output', str(output), '--hash-models']
        run = subprocess.run(cmd, env=env, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        text = output.read_text()
        data = json.loads(text)
        for excluded in ['PRIVATE_MEMORY_SHOULD_NOT_APPEAR', 'AIBOM_DUMMY_SECRET_CANARY', 'UNRELATED_SECRET_TOKEN']:
            self.assertNotIn(excluded, text)
        self.assertEqual(data['current_shell_flags']['ATHENA_R26'], '1')
        self.assertFalse(marker.exists())
        self.assertEqual(output.stat().st_mode & 0o777, 0o600)
        item = next(x for x in data['model_files'] if x['component_ref'] == 'M-whisper')
        self.assertEqual(item['sha256'], hashlib.sha256(model.read_bytes()).hexdigest())
        second = subprocess.run(cmd, env=env, capture_output=True, text=True)
        self.assertNotEqual(second.returncode, 0)
        self.assertEqual(output.read_text(), text)

if __name__ == '__main__':
    unittest.main(verbosity=2)
