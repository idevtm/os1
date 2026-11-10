#!/usr/bin/env python3
"""Compare extracted run-tests directories with versioned Linux hardware profiles."""
import argparse
import hashlib
import io
import os
import stat
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile

VERSION = 1
HEADER = struct.Struct('<8sIIIIQ')
RECORD = struct.Struct('<IIIIQ22Q96s32s32s')
ROOT = Path(__file__).resolve().parents[1]
PROFILES = ROOT / 'docs/hardware/reference-captures'
LIMIT = 4 * 1024 * 1024


def bounded(path, limit=LIMIT):
    # Nonblocking open and fstat prevent special files from bypassing the byte bound.
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise ValueError('capture input is not a regular file')
        data = source.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f'file exceeds {limit} bytes: {path}')
    return data


def digest(data):
    return hashlib.sha256(data).hexdigest()


def key_values(data):
    result = {}
    for line in data.decode('utf-8').splitlines():
        if '=' not in line:
            raise ValueError('invalid key/value record')
        key, value = line.split('=', 1)
        if key in result:
            raise ValueError('duplicate metadata key: ' + key)
        result[key] = value
    return result


def catalog(data):
    if len(data) < HEADER.size:
        raise ValueError('short catalog header')
    magic, version, size, count, complete, generation = HEADER.unpack_from(data)
    if (magic != b'OS1CAT1\0' or version != 1 or size != RECORD.size or count > 160
            or complete != 1 or generation == 0 or len(data) != HEADER.size + count * size):
        raise ValueError('invalid or incomplete catalog')
    records, ids = [], set()
    for offset in range(HEADER.size, len(data), size):
        identity, parent, source, backing, known, *fields = RECORD.unpack_from(data, offset)
        if source not in (1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14) or identity in ids:
            raise ValueError('invalid or duplicate catalog identity')
        if identity < 0x100000 or (identity & 63) or (identity - 0x100000) >> 16 != source:
            raise ValueError('catalog identity/source mismatch')
        ids.add(identity)
        text = []
        for raw in fields[22:]:
            if b'\0' not in raw:
                raise ValueError('unterminated catalog string')
            text.append(raw.split(b'\0', 1)[0].decode('ascii'))
        values = [value if known & (1 << (i + 8)) else None
                  for i, value in enumerate(fields[:22])]
        records.append(dict(id=identity, parent=parent, source=source, backing=backing,
                            values=values, name=text[0], state=text[1], driver=text[2]))
    return records


def captures(path):
    # No archive extraction: reject links, paths, duplicates, and oversized members.
    data = bounded(path)
    result = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as archive:
        for member in archive:
            if (not member.isfile() or not re.fullmatch(r'[a-z0-9_-]+\.(?:bin|txt)', member.name)
                    or member.name in result or len(result) == 32 or member.size > 262144):
                raise ValueError('invalid archive member')
            result[member.name] = archive.extractfile(member).read(262145)
    return result


def normalize(machine, base=PROFILES):
    folder = base / machine / 'linux/os1-hardware'
    pci_data = bounded(folder / 'pci.txt')
    usb_data = bounded(folder / 'usb-descriptors.txt')
    pci = []
    pattern = re.compile(r'^(?:([0-9a-f]{4}):)?([0-9a-f]{2}):([0-9a-f]{2})\.([0-7]) (.*?) \[([0-9a-f]{4})\]:.*?\[([0-9a-f]{4}):([0-9a-f]{4})\]', re.M)
    for m in pattern.finditer(pci_data.decode()):
        domain, bus, slot, function, name, cls, vendor, device = m.groups()
        pci.append(dict(location=[int(v, 16) for v in (domain or '0', bus, slot, function)],
                        vendor=int(vendor, 16), device=int(device, 16),
                        class_code=int(cls[:2], 16), subclass=int(cls[2:], 16), name=name))
    if len(pci) != len(re.findall(r'^(?:[0-9a-f]{4}:)?[0-9a-f]{2}:[0-9a-f]{2}\.[0-7] ', pci_data.decode(), re.M)):
        raise ValueError('unparsed PCI reference entry')
    usb, current, interface = [], None, None
    for line in usb_data.decode().splitlines():
        m = re.match(r'Bus .* ID ([0-9a-f]{4}):([0-9a-f]{4}) (.*)', line)
        if m:
            vendor, product, name = m.groups()
            current = None if vendor == '1d6b' and product in ('0001', '0002', '0003') else dict(
                vendor=int(vendor, 16), product=int(product, 16), name=name.strip(), interfaces=[])
            if current is not None:
                usb.append(current)
            interface = None
        elif current is not None:
            m = re.match(r'\s*bInterface(Class|SubClass|Protocol)\s+(\d+)', line)
            if m:
                field, value = m.groups()
                if field == 'Class':
                    interface = [int(value)]
                elif interface is not None:
                    interface.append(int(value))
                    if field == 'Protocol' and len(interface) == 3:
                        current['interfaces'].append(interface)
                        interface = None
    if not pci or not usb:
        raise ValueError('reference lacks numeric PCI or USB identities')
    # Reviewed platform-specific expectations. Removable peripherals remain optional.
    required = {'laptop': {'8087:0024', '13d3:5134', '0bda:0139', '0cf3:3005'},
                'desktop': {'8087:8008', '8087:8000', '17ef:6190'}}[machine]
    for device in usb:
        device['required'] = f"{device['vendor']:04x}:{device['product']:04x}" in required
        device['interfaces'] = sorted(set(tuple(x) for x in device['interfaces']))
    functions = ([{'kind': 1, 'transport': 'ps2'}, {'kind': 2, 'transport': 'ps2'}]
                 if machine == 'laptop' else [{'kind': 1, 'usb': '17ef:6190'}])
    anchors = [f"{p['vendor']:04x}:{p['device']:04x}" for p in pci
               if p['class_code'] == 6 and p['subclass'] == 0 or
               p['class_code'] == 12 and p['subclass'] == 3][:3]
    storage_lines = bounded(folder / 'storage.txt').decode().splitlines()
    columns = {key: storage_lines[0].index(key) for key in ('TYPE', 'SIZE', 'FSTYPE', 'MODEL', 'TRAN')}
    storage = []
    for line in storage_lines[1:]:
        prefix = line[:columns['FSTYPE']].split()
        if len(prefix) == 3 and prefix[1] in ('disk', 'rom'):
            storage.append(dict(model=line[columns['MODEL']:columns['TRAN']].strip(),
                transport=line[columns['TRAN']:].strip(), size_label=prefix[2],
                expectation='unsupported'))
    return dict(version=1, machine=machine, anchors=anchors, pci=pci, usb=usb, storage=storage,
                required_functions=functions,
                source_hashes={str((folder / name).relative_to(base)): digest(bounded(folder / name))
                               for name in ('pci.txt', 'usb-descriptors.txt', 'input.txt', 'storage.txt')})


def input_devices(data):
    devices, seen = [], set()
    for part in re.split(rb'(?m)(?=^device=)', data):
        if not part:
            continue
        row = key_values(part)
        if set(row) != {'device', 'name', 'kind', 'catalog_id', 'connected'}:
            raise ValueError('invalid input record fields')
        numeric = ('device', 'kind', 'catalog_id', 'connected')
        if any(not re.fullmatch(r'[0-9]{1,20}', row[key]) for key in numeric):
            raise ValueError('invalid input record integer')
        device = {key: int(row[key]) for key in numeric}
        identity = device['device']
        catalog_id = device['catalog_id']
        if (not 1 <= identity <= 64 or identity in seen or device['kind'] not in (1, 2)
                or device['connected'] not in (0, 1) or len(devices) == 64
                or catalog_id and not (0x150000 <= catalog_id < 0x160000 and catalog_id % 64 == 0)):
            raise ValueError('invalid input record identity or state')
        seen.add(identity)
        devices.append(device)
    return devices


USB_FAILURE = struct.Struct('<3Q12I')
USB_FAILURE_REASONS = {1: 'transfer', 2: 'controller', 3: 'root_change',
                       4: 'hub_change', 5: 'stop', 6: 'disconnected'}


def usb_input_failures(data):
    if len(data) < 16:
        raise ValueError('short USB input failure snapshot')
    version, kind, size, count = struct.unpack_from('<4I', data)
    if (version, kind, size) != (1, 14, USB_FAILURE.size) or count > 64 or len(data) != 16 + count * size:
        raise ValueError('invalid USB input failure snapshot')
    names = ('sequence', 'time_ns', 'input_device', 'catalog_id', 'reason', 'pci_index',
             'root_port', 'parent_address', 'port', 'address', 'endpoint', 'token',
             'expected_bytes', 'status', 'flags')
    records, seen = [], set()
    for index in range(count):
        record = dict(zip(names, USB_FAILURE.unpack_from(data, 16 + index * size)))
        identity = record['input_device']
        if (record['sequence'] != index + 1 or not 1 <= identity <= 64 or identity in seen
                or record['reason'] not in USB_FAILURE_REASONS or record['flags'] & ~1):
            raise ValueError('invalid USB input failure record')
        record['reason_name'] = USB_FAILURE_REASONS[record['reason']]
        seen.add(identity)
        records.append(record)
    return records


def compare(run, profiles, machine=None):
    manifest = key_values(bounded(run / 'manifest.txt', 16384))
    summary = key_values(bounded(run / 'summary.txt', 16384))
    for key, length in (('kernel_revision', 40), ('kernel_build', 64)):
        if not re.fullmatch(f'[0-9a-f]{{{length}}}', manifest.get(key, '')):
            raise ValueError('invalid build identity')
    members = captures(run / 'captures.tar')
    records = catalog(members['catalog.bin'])
    active = [r for r in records if r['state'] not in ('unavailable', 'disconnected')]
    pci = [r for r in active if r['source'] == 1]
    usb = [r for r in active if r['source'] == 5]
    def pair_known(r, first):
        return all(type(x) is int for x in r['values'][first:first+2])
    identities = {f"{r['values'][4]:04x}:{r['values'][5]:04x}" for r in pci if pair_known(r, 4)}
    choices = [p for p in profiles if (p['machine'] == machine if machine else
                                      set(p['anchors']) <= identities)]
    report = dict(comparison_version=VERSION, run=str(run), manifest=manifest, summary=summary,
                  capture_sha256=digest(bounded(run / 'captures.tar')), findings=[])
    if manifest.get('runner_version') == '3':
        report['usb_input_failures'] = {
            phase: usb_input_failures(members['usb-input-' + phase + '.bin'])
            for phase in ('before', 'after')}
    issues = report['findings']
    def add(status, check, **detail):
        issues.append(dict(status=status, check=check, **detail))
    for r in pci + usb:
        if not pair_known(r, 4 if r['source'] == 1 else 0):
            add('INCONCLUSIVE', 'hardware_identity_unavailable', actual=r)
    if summary.get('state') != 'complete' or summary.get('io_failed') != '0':
        add('FAIL', 'result_persistence', actual=summary)
    if int(summary.get('failed', '-1')) != 0:
        add('FAIL', 'on_device_tests', failed=summary.get('failed'))
    if len(choices) != 1:
        add('INCONCLUSIVE', 'machine_profile', candidates=[p['machine'] for p in choices])
    else:
        profile = choices[0]
        report['machine'] = profile['machine']
        report['profile_sha256'] = digest(json.dumps(profile, sort_keys=True).encode())
        remaining = list(pci)
        for expected in profile['pci']:
            matching = [r for r in remaining if r['values'][4:6] == [expected['vendor'], expected['device']]]
            selected = next((r for r in matching if r['values'][:4] == expected['location']),
                            matching[0] if matching else None)
            if selected is None:
                add('FAIL', 'missing_pci', expected=expected)
                continue
            remaining.remove(selected)
            add('PASS' if selected['values'][6:8] == [expected['class_code'], expected['subclass']]
                else 'FAIL', 'pci_classification', expected=expected, actual=selected)
            if selected['values'][:4] != expected['location']:
                add('REVIEW', 'pci_location_changed', expected=expected['location'], actual=selected['values'][:4])
        for record in remaining:
            add('REVIEW', 'unexpected_pci', actual=record)
        remaining = list(usb)
        for expected in profile['usb']:
            selected = next((r for r in remaining if r['values'][:2] == [expected['vendor'], expected['product']]), None)
            if selected is None:
                add('FAIL' if expected['required'] else 'SKIP', 'missing_usb', expected=expected)
                continue
            remaining.remove(selected)
            interfaces = {tuple(r['values'][2:5]) for r in active
                          if r['source'] == 6 and r['backing'] == selected['id']
                          and all(type(v) is int for v in r['values'][2:5])}
            expected_interfaces = {tuple(x) for x in expected['interfaces']}
            add('PASS' if expected_interfaces <= interfaces else 'FAIL', 'usb_classification',
                expected=expected, actual=selected, interfaces=sorted(interfaces))
        for record in remaining:
            add('REVIEW', 'unexpected_usb', actual=record)
        inputs = input_devices(members['input.txt'])
        progress = bounded(run / 'progress.txt', 65536).decode()
        api_pass = 'PASS input acquisition/state/subscriptions status=0 ' in progress
        for requirement in profile['required_functions']:
            candidates = [d for d in inputs if d['kind'] == requirement['kind'] and d['connected']]
            if requirement.get('transport') == 'ps2':
                candidates = [d for d in candidates if d['catalog_id'] == 0]
            else:
                ids = {r['id'] for r in usb if pair_known(r, 0) and f"{r['values'][0]:04x}:{r['values'][1]:04x}" == requirement['usb']}
                candidates = [d for d in candidates if d['catalog_id'] in ids]
            add('PASS' if candidates and api_pass else 'FAIL', 'required_input_api', expected=requirement)
        for expected in profile.get('storage', []):
            add('UNSUPPORTED', 'physical_storage_reference', expected=expected,
                reason='Internal storage identity and capacity are not yet qualified by this profile')
        for record in active:
            if record['source'] in (1, 6) and not record['driver']:
                add('UNSUPPORTED', 'unbound_function', actual=record)
    statuses = {item['status'] for item in issues}
    report['status'] = ('FAIL' if 'FAIL' in statuses else 'INCONCLUSIVE' if 'INCONCLUSIVE' in statuses
                        else 'REVIEW' if 'REVIEW' in statuses else 'PASS')
    return report


def load_profiles(root):
    profiles = []
    for path in sorted(root.glob('*/profile.json')):
        profile = json.loads(bounded(path, 262144))
        if not isinstance(profile, dict) or set(profile) != {
                'version', 'machine', 'anchors', 'pci', 'usb', 'required_functions', 'source_hashes', 'storage'}:
            raise ValueError('invalid reference profile fields')
        if (type(profile['version']) is not int or profile['version'] != 1
                or profile['machine'] != path.parent.name
                or any(not isinstance(profile[key], list) for key in
                       ('anchors', 'pci', 'usb', 'required_functions', 'storage'))
                or not isinstance(profile['source_hashes'], dict)):
            raise ValueError('invalid reference profile types')
        if (not 2 <= len(profile['anchors']) <= 8 or not 1 <= len(profile['pci']) <= 256
                or len(profile['usb']) > 128):
            raise ValueError('invalid reference profile limits')
        def integers(values, bounds):
            return isinstance(values, list) and len(values) == len(bounds) and all(type(v) is int and 0 <= v <= limit for v, limit in zip(values, bounds))
        if not all(isinstance(x, str) and re.fullmatch('[0-9a-f]{4}:[0-9a-f]{4}', x) for x in profile['anchors']):
            raise ValueError('invalid machine anchors')
        for item in profile['pci']:
            if (not isinstance(item, dict) or set(item) != {'location', 'vendor', 'device', 'class_code', 'subclass', 'name'}
                    or not integers(item['location'], [65535, 255, 31, 7])
                    or not integers([item[x] for x in ('vendor', 'device', 'class_code', 'subclass')], [65535, 65535, 255, 255])):
                raise ValueError('invalid PCI expectation')
        for item in profile['usb']:
            if (not isinstance(item, dict) or set(item) != {'vendor', 'product', 'name', 'interfaces', 'required'}
                    or not integers([item['vendor'], item['product']], [65535, 65535])
                    or type(item['required']) is not bool or not isinstance(item['interfaces'], list)
                    or len(item['interfaces']) > 256
                    or not all(integers(x, [255, 255, 255]) for x in item['interfaces'])):
                raise ValueError('invalid USB expectation')
        if len(profile['storage']) > 32 or any(not isinstance(x, dict) or set(x) != {'model', 'transport', 'size_label', 'expectation'}
                or x['expectation'] != 'unsupported' for x in profile['storage']):
            raise ValueError('invalid storage expectation')
        if len(profile['required_functions']) > 32:
            raise ValueError('too many required functions')
        for item in profile['required_functions']:
            if not isinstance(item, dict) or type(item.get('kind')) is not int or item['kind'] not in (1, 2) or not (
                    set(item) == {'kind', 'transport'} and item['transport'] == 'ps2' or
                    set(item) == {'kind', 'usb'} and isinstance(item['usb'], str) and re.fullmatch('[0-9a-f]{4}:[0-9a-f]{4}', item['usb'])):
                raise ValueError('invalid functional expectation')
        expected_sources = {f"{profile['machine']}/linux/os1-hardware/{name}"
                            for name in ('pci.txt', 'usb-descriptors.txt', 'input.txt', 'storage.txt')}
        if set(profile['source_hashes']) != expected_sources:
            raise ValueError('reference source hashes are incomplete')
        for name, expected in profile['source_hashes'].items():
            if not isinstance(expected, str) or not re.fullmatch('[0-9a-f]{64}', expected):
                raise ValueError('invalid reference source hash')
            source = (root / name).resolve()
            if not source.is_relative_to(root.resolve()) or digest(bounded(source)) != expected:
                raise ValueError('reference source changed: ' + name)
        profiles.append(profile)
    return profiles


def analyze(root, profiles, machine=None):
    manifests = [root / 'manifest.txt'] if (root / 'manifest.txt').exists() else sorted(root.rglob('manifest.txt'))
    if not manifests or len(manifests) > 256:
        raise ValueError('expected 1 through 256 captured runs')
    results = []
    for manifest in manifests:
        try:
            if not manifest.parent.resolve().is_relative_to(root.resolve()):
                raise ValueError('capture directory leaves the input root')
            results.append(compare(manifest.parent, profiles, machine))
        except (ValueError, OSError, KeyError, TypeError, struct.error, tarfile.TarError) as error:
            row = dict(run=str(manifest.parent), status='INCONCLUSIVE', error=str(error))
            try:
                summary = key_values(bounded(manifest.parent / 'summary.txt', 16384))
                row['summary'] = summary
                if summary.get('failed') != '0' or summary.get('io_failed') != '0':
                    row['status'] = 'FAIL'
            except (ValueError, OSError):
                pass  # The catalog error and missing completion evidence remain explicit.
            results.append(row)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs', nargs='?', type=Path)
    parser.add_argument('--image', type=Path, help='Read-only ext2 partition image, not a whole USB image')
    parser.add_argument('--profiles', type=Path, default=PROFILES)
    parser.add_argument('--machine', choices=('laptop', 'desktop'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--normalize', choices=('laptop', 'desktop'))
    args = parser.parse_args()
    try:
        if args.normalize:
            result = normalize(args.normalize, args.profiles)
        else:
            profiles = load_profiles(args.profiles)
            if args.image:
                debugfs = shutil.which('debugfs') or '/opt/homebrew/opt/e2fsprogs/sbin/debugfs'
                with tempfile.TemporaryDirectory(prefix='os1-compare-') as temporary:
                    subprocess.run([debugfs, '-R', f'rdump /tests {temporary}', str(args.image.resolve())],
                                   check=True, capture_output=True, timeout=60)
                    result = analyze(Path(temporary), profiles, args.machine)
                    for row in result:
                        row['run'] = str(Path(row['run']).relative_to(temporary))
                        row['source_image'] = str(args.image.resolve())
            elif args.runs:
                result = analyze(args.runs, profiles, args.machine)
            else:
                parser.error('provide a run directory or --image')
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
        if isinstance(result, list):
            for row in result:
                print(row['status'], row.get('machine', 'unmatched'), row['run'])
            return 1 if any(row['status'] == 'FAIL' for row in result) else 2 if any(row['status'] != 'PASS' for row in result) else 0
        return 0
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(2, f'comparison failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
