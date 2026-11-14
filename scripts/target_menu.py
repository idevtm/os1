#!/usr/bin/env python3
"""Navigate CMake's target manifest by category; print only the selected target."""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from write_usb import choose_menu


def category(target):
    if target in ('usb', 'usb_capture'):
        return ('USB',)
    if target.startswith('storage_'):
        return ('Storage',)
    if target.startswith('run'):
        return ('Run QEMU', 'Legacy BIOS' if 'bios' in target else 'Modern UEFI')
    if target.startswith('smoke'):
        if target == 'smoke_all':
            return ('Checks',)
        if target in ('smoke_storage', 'smoke_usb_storage'):
            return ('Checks', 'Storage')
        return ('Checks', 'Legacy BIOS' if 'bios' in target else 'Modern UEFI')
    return ('Other',)


def read_manifest(path):
    records = {}
    for line in Path(path).read_text().splitlines():
        fields = line.split('|')
        if len(fields) != 4 or not fields[0] or fields[0] in records:
            raise ValueError('invalid or duplicate target menu record')
        records[fields[0]] = fields[1:]
    if not records:
        raise ValueError('target menu manifest is empty')
    return records


def choose_target(records):
    tree = {name: {} for name in ('Run QEMU', 'Storage', 'USB', 'Checks')}
    for target, (label, description, unavailable) in records.items():
        node = tree
        for component in category(target):
            node = node.setdefault(component, {})
        node[target] = (label, description, unavailable)
    stack = [((), tree)]
    while stack:
        path, node = stack[-1]
        options, descriptions = [], {}
        for key, value in node.items():
            if isinstance(value, dict):
                if value:
                    options.append((key, key + ' >'))
                    descriptions[key] = 'Open ' + key
            else:
                label, description, unavailable = value
                options.append((key, label + (' [unavailable]' if unavailable else '')))
                descriptions[key] = unavailable or description
        selection = choose_menu('os1' + (' / ' + ' / '.join(path) if path else ''), options,
                                cancel_label='Back' if path else 'Quit', descriptions=descriptions)
        if selection is None:
            stack.pop()
            continue
        value = node[selection]
        if isinstance(value, dict):
            stack.append((path + (selection,), value))
        elif not value[2]:
            return selection
        else:
            choose_menu('Target unavailable', [], value[2], cancel_label='Back')
    return None


def main():
    try:
        target = choose_target(read_manifest(sys.argv[1]))
        if target is None:
            return 130
        print(target)
        return 0
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
