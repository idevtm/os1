#!/usr/bin/env python3
"""Read version 1 os1 log frames. Print JSON Lines and reject invalid tails."""
import argparse
import json
import struct
import sys
import zlib

HEADER = struct.Struct('<Q6I10Q2I40s64s32s')
RECORD = struct.Struct('<11Qq6I136s')
MAGIC = 0x31474F4C31534F
FIELDS = ('sequence time_ns session_id command_id pid request_id interface_id arg0 arg1 '
          'parent_command_id duration_ns status kind level flags cpu text_bytes reserved text').split()


def text(value):
    return value.split(b'\0', 1)[0].decode('utf-8', errors='replace')


def frames(source):
    """Yield complete frames. A bad tail raises ValueError after the valid prefix."""
    offset = 0
    while True:
        raw = source.read(256)
        if not raw:
            return
        if len(raw) != 256:
            raise ValueError(f'incomplete header at byte {offset}')
        (magic, version, size, record_size, count, stream, checksum, boot, session,
         segment, first, last, monotonic, wall, resolution, overwritten, dropped,
         diagnostic, boot_mode, revision, image, reserved) = HEADER.unpack(raw)
        expected = 256 if stream == 1 else 2048
        if (magic != MAGIC or version != 1 or not 256 <= size <= 65536 or
                stream not in (1, 2) or record_size != expected or
                count > (65536 - 256) // expected or size != 256 + count * expected or
                not boot or diagnostic > 1 or boot_mode > 2 or any(reserved) or
                (stream == 1 and session) or (count and (not first or last < first)) or
                (not count and (first or last))):
            raise ValueError(f'invalid header at byte {offset}')
        payload = source.read(size - 256)
        if len(payload) != size - 256:
            raise ValueError(f'incomplete frame at byte {offset}')
        checked = raw[:28] + bytes(4) + raw[32:] + payload
        if zlib.crc32(checked) != checksum:
            raise ValueError(f'checksum mismatch at byte {offset}')
        rows = []
        for at in range(0, len(payload), expected):
            row = dict(zip(FIELDS, RECORD.unpack_from(payload, at)))
            if (row['reserved'] or row['text_bytes'] > 136 or
                    row['sequence'] < first or row['sequence'] > last or
                    (rows and row['sequence'] <= rows[-1]['sequence'])):
                raise ValueError(f'invalid record at byte {offset + 256 + at}')
            del row['reserved']
            row['text'] = row['text'][:row['text_bytes']].decode('utf-8', errors='replace')
            if stream == 2:
                row['command'] = text(payload[at + 256:at + 1280])
                row['cwd'] = text(payload[at + 1280:at + 1792])
                row['completed_ns'] = struct.unpack_from('<Q', payload, at + 1792)[0]
                if any(payload[at + 1800:at + 2048]):
                    raise ValueError(f'invalid history padding at byte {offset + 256 + at}')
            rows.append(row)
        if rows and (rows[0]['sequence'] != first or rows[-1]['sequence'] != last):
            raise ValueError(f'sequence bounds mismatch at byte {offset}')
        yield {'offset': offset, 'bytes': size, 'boot': boot, 'session': session,
               'segment': segment, 'stream': 'kernel' if stream == 1 else 'shell',
               'monotonic_ns': monotonic, 'wall_ns': wall, 'resolution_ns': resolution,
               'overwritten': overwritten, 'dropped': dropped, 'diagnostic': diagnostic,
               'boot_mode': boot_mode, 'revision': text(revision), 'image': text(image),
               'records': rows}
        offset += size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+')
    args = parser.parse_args()
    failed = False
    for path in args.files:
        try:
            with open(path, 'rb') as source:
                for frame in frames(source):
                    print(json.dumps({'file': path, **frame}, ensure_ascii=True))
        except (OSError, ValueError) as error:
            print(f'{path}: {error}; preceding complete frames remain usable', file=sys.stderr)
            failed = True
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
