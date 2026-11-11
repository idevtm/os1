#!/usr/bin/env python3
"""Add the current driver package to the existing preserving storage manifest."""
import argparse
import json
from pathlib import Path
from driver_package import write_if_changed

parser = argparse.ArgumentParser()
for name in ('base', 'metadata', 'image', 'output'):
    parser.add_argument('--' + name, required=True,
                        action='append' if name in ('metadata', 'image') else 'store')
args = parser.parse_args()
data = json.loads(Path(args.base).read_text())
if len(args.metadata) != len(args.image):
    parser.error('each driver needs metadata and an image')
for metadata, image in zip(args.metadata, args.image):
    module = json.loads(Path(metadata).read_text())
    data['programs'].append({'source': str(Path(image).absolute()), 'destination': module['image']})
    data['resources'] += [
        {'source': str(Path(metadata).absolute()),
         'destination': str(Path(module['image']).parent / 'manifest.json')}]
write_if_changed(args.output, json.dumps(data, indent=2) + '\n')
