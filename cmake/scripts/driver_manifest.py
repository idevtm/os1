#!/usr/bin/env python3
"""Add the current driver package to the existing preserving storage manifest."""
import argparse
import json
from pathlib import Path
from driver_package import write_if_changed

parser = argparse.ArgumentParser()
for name in ('base', 'metadata', 'image', 'output'):
    parser.add_argument('--' + name, required=True)
args = parser.parse_args()
data = json.loads(Path(args.base).read_text())
module = json.loads(Path(args.metadata).read_text())
data['programs'].append({'source': str(Path(args.image).absolute()), 'destination': module['image']})
data['resources'] += [
    {'source': str(Path(args.metadata).absolute()),
     'destination': str(Path(module['image']).parent / 'manifest.json')}]
write_if_changed(args.output, json.dumps(data, indent=2) + '\n')
