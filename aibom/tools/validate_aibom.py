#!/usr/bin/env python3
"""Offline schema, semantic-reference, evidence-hash and optional source validation."""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
from datetime import datetime, timezone

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON property: ' + key)
        result[key] = value
    return result

def read(path):
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique_object)

def require(condition, message):
    if not condition:
        raise ValueError(message)

def safe_relative(value):
    path = PurePosixPath(value)
    require(not path.is_absolute() and '..' not in path.parts, 'Unsafe relative path: ' + value)
    return path

def unique_ids(rows, field):
    ids = [r[field] for r in rows]
    require(len(ids) == len(set(ids)), 'Duplicate identifiers: ' + field)
    return set(ids)

def validate_licensing(aibom, bom, graph, evidence, refs):
    """Check documentary scope and exact supplied bytes, not legal enforceability."""
    mapping = read(aibom / 'license-mapping.json')
    source = read(aibom / 'licensing-source-files.json')
    require(mapping['format'] == 'Athena licensing reconciliation 1.0', 'Unknown licensing mapping format')
    require(source['format'] == 'Athena licensing source evidence manifest 1.0', 'Unknown licensing source format')
    require(bom['version'] == graph['header']['revision'] == mapping['document_revision'] == source['document_revision'], 'Licensing/document revision mismatch')
    require(bom['version'] == 3, 'This licensing validation profile is for AIBOM revision 3')
    identifier = 'LicenseRef-Athena-NCARD-2.0'
    license_name = 'Athena Noncommercial, Attribution and Responsible Deployment License 2.0'
    require(mapping['license']['custom_identifier'] == identifier and mapping['license']['name'] == license_name, 'Wrong custom license identity')
    require(mapping['license']['version'] == '2.0-only' and mapping['license']['spdx_license_list_id'] is False, 'Incorrect fixed version or SPDX-list claim')
    by_evidence = {r['id']: r for r in evidence}
    require(source['supplied_archive']['sha256'] == by_evidence['E60']['sha256'], 'Source archive evidence mismatch')
    pattern = r'[0-9a-f]{64}'
    require(re.fullmatch(pattern, source['supplied_archive']['sha256']) is not None, 'Invalid licensing archive digest')
    require(len(source['files']) == 66, 'Incomplete original licensing archive inventory')
    unique_ids(source['files'], 'source_package_path')
    copied = {}
    for row in source['files']:
        safe_relative(row['source_package_path'])
        require(re.fullmatch(pattern, row['sha256']) is not None, 'Invalid licensing source digest')
        rel = row['included_snapshot_path']
        if rel is None:
            continue
        path = aibom / safe_relative(rel)
        require(rel not in copied, 'Duplicate licensing snapshot path')
        require(path.is_file() and digest(path) == row['sha256'], 'License snapshot bytes differ: ' + rel)
        require(path.stat().st_size == row['size_bytes'], 'License snapshot size differs: ' + rel)
        require(rel == 'licensing/snapshot/' + row['source_package_path'], 'License snapshot/source path mismatch')
        copied[rel] = row
    actual = {p.relative_to(aibom).as_posix() for p in (aibom / 'licensing/snapshot').rglob('*') if p.is_file()}
    require(set(copied) == actual and len(copied) == 49, 'Licensing snapshot coverage mismatch')
    for record in evidence:
        if record['kind'] == 'user-supplied-license-document':
            require(record['url'] in copied and copied[record['url']]['sha256'] == record['sha256'], 'Licensing evidence identity mismatch')
    expected_license_hash = '194ddc206dfa98dd31580311cc3d6797335218622114a0ce007758c00de70ba3'
    require(mapping['license']['sha256'] == expected_license_hash, 'Unexpected operative v2.0 license hash')
    for folder in ['athena-upload', 'athena-consciousness-upload']:
        require(digest(aibom / 'licensing/snapshot' / folder / 'LICENSE') == expected_license_hash, 'Operative license copies differ')
    exact_text = (aibom / 'licensing/snapshot/athena-upload/LICENSE').read_bytes()
    embedded = bom['metadata']['component']['licenses'][0]['license']['text']
    require(embedded['contentType'] == 'text/plain' and embedded['content'].encode('utf-8') == exact_text, 'Embedded license text differs from supplied bytes')
    require(graph['header']['licensing']['custom_identifier'] == identifier, 'Graph licensing identity mismatch')
    require(graph['header']['licensing']['adoption_commit'] == source['adoption_commit'] == {'stock': None, 'overlay': None}, 'Revision 3 must not invent adoption commits')
    unique_ids(mapping['repositories'], 'id')
    require({r['id'] for r in mapping['repositories']} == {'stock', 'overlay'}, 'Missing repository licensing scope')
    for repo in mapping['repositories']:
        key = repo['id']
        expected = graph['header']['source_revisions'][key]['commit']
        require(repo['runtime_source_commit'] == expected, 'Current source binding mismatch')
        require(repo['adoption_commit'] is None and repo['adoption_status'] == 'not-recorded', 'Revision 3 must not infer adoption')
        for field in ['notice_path', 'scope_path', 'transition_path']:
            require(repo[field] in copied, 'Unbound repository notice/scope/transition')
        metadata_path = (aibom / repo['scope_path']).parent / 'licensing/PACKAGE-METADATA.json'
        metadata = read(metadata_path)
        require(metadata['reviewed_head'] == repo['supplied_package_reviewed_commit'] and metadata['custom_license_id'] == identifier, 'Supplied licensing-review identity mismatch')
        require(metadata['runtime_assent_or_safeguards_implemented'] is False and metadata['licensed_counsel_approval_obtained'] is False, 'Unsupported implementation/approval in source metadata')

    expected_covered = {'C-overlay', 'C-stock', 'C-brain', 'C-mind', 'C-inner', 'C-memory', 'C-vision', 'C-prosody', 'C-speech', 'C-launch', 'C-calibrate', 'C-export', 'P-prompts', 'P-config', 'P-wire', 'C-install'}
    covered = unique_ids(mapping['component_coverage'], 'component_ref')
    require(covered == expected_covered and covered <= refs, 'Licensing component scope mismatch')
    component_map = {c['bom-ref']: c for c in bom['components']}
    node_map = {n['id']: n for n in graph['nodes']}
    def properties(obj):
        unique_ids(obj.get('properties', []), 'name')
        return {p['name']: p['value'] for p in obj.get('properties', [])}
    for item in mapping['component_coverage']:
        cid = item['component_ref']
        c = component_map[cid]
        require(item['declared_license'] == identifier and item['ownership_and_history_audit'] == 'not-complete', 'Unsupported component licensing conclusion')
        require(set(item['evidence_refs']) <= set(by_evidence), 'Unresolved licensing scope evidence')
        require(len(c['licenses']) == 1 and 'license' in c['licenses'][0], 'Expected scoped named license, not MIT alternative')
        lic = c['licenses'][0]['license']
        require('id' not in lic and lic['name'] == license_name and lic['acknowledgement'] == 'declared', 'Invalid custom named-license declaration')
        require(lic['url'] in copied and properties(lic)['athena:license:identifier'] == identifier, 'Unbound component license')
        require(properties(lic)['athena:license:scope'] == item['scope'] == mapping['license']['scope'], 'Lost component-level scope limitation')
        values = properties(c)
        require(values['athena:license:identifier'] == identifier and values['athena:license:adoption-commit'] == 'not-recorded', 'Unverified component adoption')
        require(set(item['evidence_refs']) <= set(json.loads(values['athena:evidence:refs'])), 'Component lacks licensing evidence')
        require(node_map[cid]['evidence_refs'] == json.loads(values['athena:evidence:refs']), 'Graph/component licensing evidence differs')
    for c in bom['components']:
        if c['bom-ref'] not in covered:
            require(identifier not in json.dumps(c.get('licenses', [])), 'Athena license incorrectly applied to excluded component')
    historical = component_map['C-stock'].get('evidence', {}).get('licenses', [])
    require(any(l.get('license', {}).get('id') == 'MIT' for l in historical), 'Historical stock MIT evidence lost')
    for item in mapping['upstream_mixed_scope']:
        require(item['component_ref'] in refs and set(item['evidence_refs']) <= set(by_evidence), 'Unresolved mixed-origin licensing scope')
    obligation_ids = unique_ids(mapping['obligations'], 'id')
    require(obligation_ids == {f'O{i:02}' for i in range(1, 16)}, 'Incomplete obligation mapping')
    for item in mapping['obligations']:
        require(bool(item['affected_refs']) and set(item['affected_refs']) <= refs, 'Unresolved licensing obligation target')
        require(bool(item['evidence_refs']) and set(item['evidence_refs']) <= set(by_evidence), 'Unresolved licensing obligation evidence')
        require(bool(item['license_sections']) and bool(item['verification_needed']), 'Missing obligation section/verification')
        require(item['status'] == 'documented-requirement; implementation-or-assent-not-attested', 'Requirement incorrectly asserted as implemented')
    require({g['id'] for g in graph['gaps']} >= {'G04', 'G13', 'G14', 'G15', 'G16'}, 'Missing residual licensing verification gaps')
    return len(copied), len(covered), len(obligation_ids)

def validate_projector(aibom, bom, graph, evidence, source, artifacts, overlay=None):
    """Cross-check the revision-3 reference pin without downloading model bytes."""
    update = read(aibom / 'source-update.json')
    require(update['format'] == 'Athena source update evidence 1.0' and update['document_revision'] == 3,
            'Unknown source-update profile')
    old = 'f779a98246d4a8cf69c2bbf6e905a1439fefd0f3'
    current = 'b3c6e54ead4976c043cd0fca9452bd07d7e2689c'
    pin = 'da33c16fa4440f831149fcf53b98a22bc07785e5'
    expected_hash = 'b3624272d7b9b49ffe6c6d0c592980bed6b026ce59cde11708bb230395c2a227'
    expected_size = 921705184
    url = 'https://huggingface.co/unsloth/Qwen3.5-397B-A17B-GGUF/resolve/' + pin + '/mmproj-BF16.gguf'
    require(update['previous_overlay_commit'] == old and update['current_overlay_commit'] == current,
            'Source-update commit mismatch')
    require(graph['header']['source_revisions']['overlay']['commit'] == current,
            'Projector inventory source commit mismatch')
    require(graph['header']['source_update']['previous_overlay_commit'] == old and
            graph['header']['source_update']['current_overlay_commit'] == current,
            'Graph source-update binding mismatch')
    require(update['native_runtime_source_changed'] is False and update['stock_source_changed'] is False,
            'Unexpected revision-3 runtime source change')
    components = {c['bom-ref']: c for c in bom['components']}
    def props(obj):
        unique_ids(obj.get('properties', []), 'name')
        return {p['name']: p['value'] for p in obj.get('properties', [])}
    projector = components['M-projector']
    values = props(projector)
    rows = [r for r in artifacts if r['component_ref'] == 'M-projector']
    require(len(rows) == 1, 'Expected one reference projector artifact')
    row = rows[0]
    declared = update['projector']
    require(projector['version'] == row['publisher_revision_observed'] == row['installer_revision_pin'] ==
            declared['publisher_revision'] == pin, 'Projector revision pin mismatch')
    hashes = [{'alg': 'SHA-256', 'content': expected_hash}]
    require(projector['hashes'] == hashes and values['athena:integrity:expected-sha256'] ==
            row['registry_reported_sha256'] == row['installer_expected_sha256'] ==
            declared['expected_sha256'] == expected_hash, 'Projector expected hash mismatch')
    require(values['athena:artifact:expected-size-bytes'] == str(expected_size) and
            row['registry_reported_size_bytes'] == row['installer_expected_size_bytes'] ==
            declared['expected_size_bytes'] == expected_size, 'Projector expected size mismatch')
    require(row['url'] == declared['url'] == url and any(
        r['type'] == 'distribution' and r['url'] == url and r.get('hashes') == hashes
        for r in projector['externalReferences']), 'Projector distribution URL/hash mismatch')
    require(row['local_reference_path'] == declared['local_reference_path'] ==
            values['athena:installation:local-path'] == 'models/mmproj-BF16.gguf',
            'Projector local reference path mismatch')
    require(row['installer_component_ref'] == values['athena:installation:provider'] == 'C-install' and
            row['installer_source_commit'] == current, 'Projector installer binding mismatch')
    require(row['installed_sha256'] is None and declared['installed_sha256'] is None and
            row['weight_bytes_downloaded'] is False and values['athena:runtime-observed'] == 'false' and
            values['athena:integrity:installed-sha256'] == 'not-captured',
            'Projector download must not become an installation observation')
    require(projector['licenses'][0]['license']['id'] == 'Apache-2.0' and
            projector['licenses'][0]['license']['acknowledgement'] == 'declared',
            'Lost independent publisher-declared projector terms')
    by_evidence = {r['id']: r for r in evidence}
    require(by_evidence['E86']['sha256'] == digest(aibom / 'source-update.json'),
            'Source-update evidence digest mismatch')
    require(by_evidence['E87']['revision'] == pin and
            by_evidence['E87']['registry_reported_sha256'] == expected_hash and
            by_evidence['E87']['registry_reported_size_bytes'] == expected_size,
            'Publisher projector pointer mismatch')
    require(set(row['evidence_refs']) <= set(by_evidence), 'Unresolved projector artifact evidence')
    overlay_rows = {r['path']: r for r in source['files'] if r['repository'] == 'overlay'}
    require(len(overlay_rows) == update['tracked_overlay_files'] == 87 and
            all(r['revision'] == current for r in overlay_rows.values()), 'Overlay manifest revision mismatch')
    require(unique_ids(update['changed_files'], 'path') == {'README.md', 'patches/install-consciousness.sh'} and
            update['unchanged_overlay_file_bytes'] == 85, 'Unexpected source-update delta')
    for changed in update['changed_files']:
        original = overlay_rows[changed['path']]
        require(changed['current_sha256'] == original['sha256'] and
                changed['current_git_blob_oid'] == original['git_blob_oid'] and
                changed['current_size_bytes'] == original['size_bytes'], 'Source-update file binding mismatch')
    for evidence_id, path in [('E01', 'README.md'), ('E24', 'patches/install-consciousness.sh')]:
        require(by_evidence[evidence_id]['revision'] == current and
                by_evidence[evidence_id]['sha256'] == overlay_rows[path]['sha256'],
                'Current installer/README evidence mismatch')
    require(components['C-overlay']['pedigree']['commits'][0]['uid'] == current and
            components['C-overlay']['purl'] == 'pkg:github/igorbarshteyn/athena-consciousness@' + current and
            components['C-install']['version'] == current, 'Installer component source identity mismatch')
    nodes = {n['id']: n for n in graph['nodes']}
    for ref in ['C-install', 'S-curl-cli']:
        p = props(components[ref])
        require(p['athena:lifecycle-role'] == nodes[ref]['role'] == 'build-only' and
                nodes[ref]['zone'] == 'Z-build' and p['athena:runtime-observed'] == 'false',
                'Installer/downloader must remain build-only')
    dependencies = {d['ref']: d.get('dependsOn', []) for d in bom['dependencies']}
    require('C-install' in dependencies['C-overlay'] and
            set(dependencies['C-install']) == {'S-shell', 'S-curl-cli'}, 'Installer dependency mismatch')
    flows = {f['id']: f for f in graph['flows']}
    for fid, src, dst in [('F50', 'C-install', 'V-hf'), ('F51', 'V-hf', 'C-install'),
                          ('F52', 'C-install', 'M-projector')]:
        f = flows[fid]
        require(f['source'] == src and f['target'] == dst and f['phase'] == 'build' and
                f['observation'] == 'declared-possible; not runtime-observed',
                'Projector acquisition flow must remain build-only: ' + fid)
    lineage = [r for r in graph['lineage'] if r['artifact_ref'] == 'M-projector']
    require(len(lineage) == 1 and lineage[0]['relationship'] == 'installer-pinned-compatible-projector-distribution',
            'Obsolete candidate projector lineage')
    gap = next(g for g in graph['gaps'] if g['id'] == 'G03')
    require(gap['status'] == 'open' and 'C-install' in gap['affected_refs'],
            'Projector deployment verification gap must remain open')
    if overlay is not None:
        installer_text = (overlay / 'patches/install-consciousness.sh').read_text(encoding='utf-8')
        for name, value in [('revision', pin), ('expected_sha', expected_hash), ('expected_size', str(expected_size))]:
            require(re.search(r'^    ' + name + '=' + re.escape(value) + r'$', installer_text, flags=re.M),
                    'Actual installer pin differs: ' + name)
        require(update['readme_test_claim']['tooling_included'] is False and
                update['readme_test_claim']['public_test_path'] is None and
                not (overlay / 'patches/tests/test_install_vision_model.py').exists(),
                'Installer-test availability changed; review source-update record')

def validate(root, overlay=None, stock=None, package_checksums=True):
    try:
        import jsonschema
        from referencing import Registry, Resource
    except ImportError as error:
        raise ValueError("Install the validation dependency: python -m pip install 'jsonschema>=4.18,<5'") from error
    aibom = root / 'aibom'
    bom = read(aibom / 'athena-r26.1.cdx.json')
    graph = read(aibom / 'athena-r26.1.graph.json')
    evidence = read(aibom / 'evidence.json')['records']
    source = read(aibom / 'source-files.json')
    artifacts = read(aibom / 'model-artifacts.json')['artifacts']
    registry = Registry()
    schemas = {}
    for path in (aibom / 'schema').glob('*.json'):
        schema = read(path)
        jsonschema.Draft7Validator.check_schema(schema)
        schemas[path.name] = schema
        urls = [schema.get('$id', path.as_uri())]
        if path.name != 'athena-graph-1.0.schema.json':
            urls += ['http://cyclonedx.org/schema/' + path.name, 'https://cyclonedx.org/schema/' + path.name]
        for url in urls:
            registry = registry.with_resource(url, Resource.from_contents(schema))
    # No network retrieval callback is installed: all schema resolution is local.
    for name, document in [('bom-1.7.schema.json', bom), ('athena-graph-1.0.schema.json', graph)]:
        errors = list(jsonschema.Draft7Validator(schemas[name], registry=registry).iter_errors(document))
        if errors:
            error = errors[0]
            raise ValueError(name + ': ' + '/'.join(map(str, error.path)) + ': ' + error.message)
    items = bom['components'] + bom.get('services', []) + [bom['metadata']['component']]
    refs = unique_ids(items, 'bom-ref')
    evidence_ids = unique_ids(evidence, 'id')
    node_ids = unique_ids(graph['nodes'], 'id')
    zone_ids = unique_ids(graph['zones'], 'id')
    boundary_ids = unique_ids(graph['boundaries'], 'id')
    flow_ids = unique_ids(graph['flows'], 'id')
    for name in ['behaviors', 'lineage', 'gaps']:
        unique_ids(graph[name], 'id')
    require(node_ids == refs - {bom['metadata']['component']['bom-ref']}, 'Inventory / graph node mismatch')
    def evid(values):
        require(bool(values), 'Missing evidence references')
        require(set(values) <= evidence_ids, 'Unresolved evidence reference')
    for c in items[:-1]:
        properties = {p['name']: p['value'] for p in c.get('properties', [])}
        evid(json.loads(properties['athena:evidence:refs']))
        for dataset in c.get('modelCard', {}).get('modelParameters', {}).get('datasets', []):
            if 'ref' in dataset:
                require(dataset['ref'] in refs, 'Unresolved model-card dataset')
    node_map = {n['id']: n for n in graph['nodes']}
    for n in graph['nodes']:
        require(n['id'] == n['bom_ref'] and n['bom_ref'] in refs, 'Invalid node binding')
        require(n['zone'] in zone_ids, 'Unknown zone')
        evid(n['evidence_refs'])
    unique_ids(bom['dependencies'], 'ref')
    for d in bom['dependencies']:
        require(d['ref'] in refs and set(d.get('dependsOn', [])) <= refs, 'Unresolved dependency')
        require(d['ref'] not in d.get('dependsOn', []), 'Self dependency')
        require(len(d.get('dependsOn', [])) == len(set(d.get('dependsOn', []))), 'Duplicate dependency edge')
    bounds = {b['id']: b for b in graph['boundaries']}
    expected_crossings = {b: set() for b in boundary_ids}
    for f in graph['flows']:
        require(f['source'] in node_ids and f['target'] in node_ids, 'Unresolved flow endpoint: ' + f['id'])
        evid(f['evidence_refs'])
        require(set(f['boundary_refs']) <= boundary_ids, 'Unresolved flow boundary')
        pair = {node_map[f['source']]['zone'], node_map[f['target']]['zone']}
        require(bool(f['boundary_refs']) == (len(pair) > 1), 'Missing or spurious boundary: ' + f['id'])
        for ref in f['boundary_refs']:
            require(set(bounds[ref]['zones']) == pair, 'Boundary zones do not match flow')
            expected_crossings[ref].add(f['id'])
    for b in graph['boundaries']:
        require(set(b['zones']) <= zone_ids and len(set(b['zones'])) == 2, 'Invalid boundary zones')
        require(set(b['flow_refs']) == expected_crossings[b['id']], 'Boundary edge membership mismatch')
    for b in graph['behaviors']:
        require(b['actor_ref'] in node_ids and set(b['model_refs']) <= refs, 'Unknown behavior actor/model')
        evid(b['evidence_refs'])
    for lineage in graph['lineage']:
        require(lineage['artifact_ref'] in refs and lineage['ancestor_ref'] in refs, 'Unresolved lineage')
        evid(lineage['evidence_refs'])
    for gap in graph['gaps']:
        require(set(gap['affected_refs']) <= refs, 'Unresolved gap target')
    for c in bom['compositions']:
        for field in ['assemblies', 'dependencies']:
            require(set(c.get(field, [])) <= refs, 'Unresolved composition member')
    checks = ['CycloneDX 1.7 schema', 'Athena graph 1.0 schema', 'Unique identifiers and inventory bindings',
              'Dependencies, model-card datasets and lineage', 'Directed flows and trust-boundary membership',
              'Behavior, gap and evidence references']
    for ref in bom['externalReferences']:
        if 'hashes' not in ref:
            continue
        path = aibom / safe_relative(ref['url'])
        for h in ref['hashes']:
            require(h['alg'] == 'SHA-256' and digest(path) == h['content'], 'Evidence sidecar hash mismatch: ' + str(path))
    checks.append('CycloneDX evidence-sidecar SHA-256 bindings')
    pattern = r'[0-9a-f]{64}'
    for row in source['files']:
        safe_relative(row['path'])
        require(re.fullmatch(pattern, row['sha256']) is not None, 'Invalid source SHA-256')
        require(re.fullmatch(r'[0-9a-f]{40}', row['git_blob_oid']) is not None, 'Invalid Git object ID')
    require(len({(r['repository'], r['path']) for r in source['files']}) == len(source['files']), 'Duplicate source file')
    for row in artifacts:
        require(row['component_ref'] in refs, 'Unknown model artifact component')
        safe_relative(row['local_reference_path'])
        require(row['evidence_ref'] in evidence_ids, 'Unknown artifact evidence')
        value = row.get('registry_reported_sha256')
        if value is not None:
            require(re.fullmatch(pattern, value) is not None, 'Invalid registry SHA-256')
            require('/' + row['publisher_revision_observed'] + '/' in row['url'], 'Unpinned artifact reference URL')
        require(row['installed_sha256'] is None, 'Reference manifest must not silently become an installed manifest')
    require(len([r for r in artifacts if r['component_ref'] == 'M-qwen']) == 5, 'Expected five reference Qwen shards')
    checks.append('Source and model-artifact manifest integrity rules')
    licensing_files, licensing_components, licensing_obligations = validate_licensing(aibom, bom, graph, evidence, refs)
    validate_projector(aibom, bom, graph, evidence, source, artifacts, overlay)
    checks += ['Supplied licensing snapshot hashes and exact embedded license text',
               'Scoped custom license, historical MIT and independent component boundaries',
               'Current-source and original licensing-review bindings; unverified adoption/assent status',
               'Licensing obligation/evidence mappings and residual verification gaps',
               'Projector publisher/installer pin, size, digest and source-delta cross-bindings',
               'Build-only acquisition flows and unobserved deployment boundaries']
    checked_sources = 0
    for key, directory in [('overlay', overlay), ('stock', stock)]:
        if directory is None:
            continue
        directory = directory.resolve()
        head = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
        expected = graph['header']['source_revisions'][key]['commit']
        require(head == expected, key + ' checkout is not at the recorded source commit')
        for row in source['files']:
            if row['repository'] != key:
                continue
            path = directory / safe_relative(row['path'])
            require(path.is_file() and digest(path) == row['sha256'], 'Source bytes differ: ' + key + '/' + row['path'])
            require(path.stat().st_size == row['size_bytes'], 'Source size differs')
            checked_sources += 1
    if checked_sources:
        checks.append('Exact source checkout bytes (' + str(checked_sources) + ' files)')
    checked_package_files = 0
    if package_checksums:
        path = aibom / 'SHA256SUMS'
        require(path.is_file(), 'Missing package checksum manifest')
        seen = set()
        for line in path.read_text().splitlines():
            value, name = line.split('  ', 1)
            safe_relative(name)
            require(name not in seen, 'Duplicate package checksum path')
            seen.add(name)
            require(re.fullmatch(pattern, value) is not None, 'Invalid package SHA-256')
            file = root / name
            require(file.is_file() and digest(file) == value, 'Package bytes differ: ' + name)
            checked_package_files += 1
        expected = {'AIBOM.md', 'UPLOAD.md'} | {str(p.relative_to(root)) for p in aibom.rglob('*') if p.is_file() and p != path and '__pycache__' not in p.parts}
        require(seen == expected, 'Package manifest omits files or includes unexpected paths')
        checks.append('Final package SHA-256 checksums (' + str(checked_package_files) + ' files)')
    return {'status': 'passed', 'scope': 'AIBOM documents and optional source snapshots only; no runtime/model evaluation or license clearance',
            'validated_at': datetime.now(timezone.utc).isoformat(), 'jsonschema_version': importlib.metadata.version('jsonschema'),
            'components': len(bom['components']), 'services': len(bom['services']), 'flows': len(graph['flows']),
            'evidence_records': len(evidence), 'source_files_checked': checked_sources,
            'licensing_snapshot_files_checked': licensing_files, 'licensing_components_checked': licensing_components,
            'licensing_obligations_checked': licensing_obligations,
            'package_files_checked': checked_package_files, 'checks': checks}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--overlay', type=Path)
    parser.add_argument('--stock', type=Path)
    parser.add_argument('--report', type=Path, help='Write a new validation report; rehash the package after intentional report updates.')
    parser.add_argument('--skip-package-checksums', action='store_true', help='For intentional authoring before final package checksums exist.')
    args = parser.parse_args()
    try:
        result = validate(args.root.resolve(), args.overlay, args.stock, not args.skip_package_checksums)
        if args.report:
            args.report.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
