#!/usr/bin/env python3
"""Text aliases, typed arguments, retained inputs, and strict scripts in a guest."""
import argparse
import shutil
import sys
from pathlib import Path
sys.dont_write_bytecode = True

from test_ext2_persistence import boot


def exercise(run, expect):
    def ok(command, marker=None):
        result = run(command, marker)
        assert b'failed' not in result and b'shell:' not in result, result
        return result

    def bad(command, marker):
        result = run(command, allow_errors=True)
        assert marker in result, result
        return result

    def content(path, value):
        result = ok(f'cat "{path}"')
        # Console command echo precedes the content and the next prompt follows it.
        assert result.endswith(value + b'os1> '), result

    ok('cd "/tmp"')
    ok('write "sample"\nalpha one\nBeta two\nalpha.* three\n.')
    for command, expected in [
        ('head "sample" lines:1', b'alpha one\n'),
        ('text.head lines:2 "sample"', b'alpha one\nBeta two\n'),
        ('tail "sample" lines:1', b'alpha.* three\n'),
        ('text.tail source:"sample" lines:2', b'Beta two\nalpha.* three\n'),
        ('head "sample"', b'alpha one\nBeta two\nalpha.* three\n'),
        ('tail "sample" lines:0', b''),
        ('grep "alpha" "sample"', b'alpha one\nalpha.* three\n'),
        ('text.search source:"sample" "Beta"', b'Beta two\n'),
        ('grep ".*" "sample"', b'alpha.* three\n'),
        ('grep "" "sample"', b'alpha one\nBeta two\nalpha.* three\n'),
        ('wc "sample"', b'lines:3 words:6 bytes:33\n'),
        ('text.count process.streams.input < "sample"', b'lines:3 words:6 bytes:33\n'),
        ('head - lines:1 < "sample"', b'alpha one\n'),
        ('tail lines:1 < "sample"', b'alpha.* three\n'),
        ('grep "Beta" < "sample"', b'Beta two\n'),
        ('file "sample"', b'ASCII text\n'),
        ('fs.identify "/tmp"', b'directory\n'),
        ('file "/sbin/init"', b'ELF64\n'),
        ('file "/bin/diskutil"', b'ELF64\n'),
    ]:
        ok(command + ' > "out"')
        content('out', expected)
    ok('touch "empty"')
    ok('file "empty"', b'empty\n')
    ok('echo "literal dash" > "-"')
    ok('head "-"', b'literal dash\n')
    ok('head lines:1 << END > "here"\nfirst\nsecond\nEND')
    content('here', b'first\n')
    for command in ['head', 'tail', 'wc', 'file', 'grep "alpha"']:
        bad(command + ' "sample" > "sample"', b'redirection failed')
        content('sample', b'alpha one\nBeta two\nalpha.* three\n')
    ok('echo "preserved" > "guard"')
    for command in ['head "sample" lines:-1', 'tail "sample" lines:true',
                    'head "sample" lines:"2"', 'head "missing"',
                    'wc process.streams.output']:
        bad(command + ' > "guard"', b'shell:')
        content('guard', b'preserved\n')
    ok('write "strict.sh"\ntext.head lines:1 "sample"\ntext.search "Beta" source:"sample"\ntext.count "sample"\n.')
    ok('sh "strict.sh" > "script-out"')
    content('script-out', b'alpha one\nBeta two\nlines:3 words:6 bytes:33\n')
    ok('write "no-match.sh"\ngrep "absent" "sample"\necho "must-not-run"\n.')
    result = bad('sh "no-match.sh"', b'status=1')
    assert b'must-not-run\n' not in result and b'shell:' not in result, result
    result = ok('grep "absent" "sample"')
    assert result.endswith(b'os1> '), result
    ok('echo "' + 'x' * 80 + '" > "/long"')
    # Chunk-boundary coverage through a real disk handle and a retained input.
    for _ in range(6):
        ok('cp "/long" "/chunk"')
        ok('cat "/chunk" >> "/long"')
    ok('wc "/long"', b'lines:64 words:64 bytes:5184\n')
    ok('tail "/long" lines:2 > "two"')
    ok('wc "two"', b'lines:2 words:2 bytes:162\n')
    ok('grep "xxx" "/long" > "matched"')
    ok('wc "matched"', b'lines:64 words:64 bytes:5184\n')
    # A redirected regular file must retain SEEK, including lines larger than the stream workspace.
    ok('write "/fragment"\n' + 'x' * 80 + '\x04\x04')
    expect()  # run() adds LF after Ctrl-D; consume that empty command's prompt.
    ok('wc "/fragment"', b'lines:0 words:1 bytes:80\n')
    for _ in range(10):
        ok('cp "/fragment" "/chunk"')
        ok('cat "/chunk" >> "/fragment"')
    ok('echo "needle" >> "/fragment"')
    for command in ['tail "/fragment" lines:1', 'tail lines:1 < "/fragment"',
                    'grep "needle" "/fragment"', 'grep "needle" < "/fragment"']:
        ok(command + ' > "/large-match"')
        ok('wc "/large-match"', b'lines:1 words:1 bytes:81927\n')
    bad('tail lines:1 < "sample" > "sample"', b'redirection failed')
    content('sample', b'alpha one\nBeta two\nalpha.* three\n')
    # A native parent can grant READ on descriptor 1; redirection must retain that source.
    result = run('textcheck', b'[user/textcheck] readable stdout input retained ok')
    assert b'[user/textcheck] fail' not in result, result
    ok('/bin/diskutil sync', b'sync ok')
    ok('kmem', b'kmem complete')


def main():
    parser = argparse.ArgumentParser()
    for name in ['qemu', 'image', 'program-disk', 'log']:
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--code')
    parser.add_argument('--vars')
    args = parser.parse_args()
    Path(args.log).parent.mkdir(parents=True, exist_ok=True)
    disk = Path(args.log).with_suffix('.raw')
    shutil.copyfile(args.program_disk, disk)
    boot(args, disk, 'text', exercise=exercise)
    print('text utilities, typed arguments, and retained-input checks passed')


if __name__ == '__main__':
    main()
