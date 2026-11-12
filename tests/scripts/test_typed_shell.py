#!/usr/bin/env python3
"""Typed context, shell aliases, namespace moves, and persistent reboot contract."""
import argparse
import shutil
from pathlib import Path
from test_ext2_persistence import boot


def exercise(run, expect):
    def ok(command, expected=None):
        result = run(command, expected)
        assert b' failed' not in result and b'unknown command' not in result, result
        return result

    ok('pwd', b'/\n')
    ok('cd "/tmp"')
    ok('mkdir "one"')
    ok('mkdir "two"')
    ok('cd "one"')
    ok('pwd', b'/tmp/one\n')
    ok('echo "payload" > "a.txt"')
    ok('echo "f(x)" > "parentheses.txt"')
    ok("echo '(' >> parentheses.txt")
    ok('cat "parentheses.txt"', b'f(x)\n(\n')
    ok('echo "f(x) > y"', b'f(x) > y\n')
    ok('cp destination:"b.txt" "a.txt"')
    ok('cat "b.txt"', b'payload\n')
    ok('mv "b.txt" "../two/new.txt"')
    ok('cat "../two/new.txt"', b'payload\n')
    ok('mv "/tmp/one" "/tmp/renamed"')
    ok('pwd', b'/tmp/renamed\n')
    ok('cd ".."')
    ok('pwd', b'/tmp\n')
    ok('touch "fresh"')
    ok('rm "fresh"')
    ok('mkdir "empty"')
    ok('rmdir "empty"')
    warned = ok('cat two/new.txt', b'payload\n')
    assert b'warning: interpreting two/new.txt as text' not in warned, warned
    ok('write "literal-path.sh"\ncat two/new.txt\n.')
    ok('sh "literal-path.sh"', b'payload\n')
    ok('echo fs.cwd', b'/tmp\n')
    ok('process.environment.build = {jobs:4, label:"parent"}')
    ok('process.environment.build', b'jobs:4')
    ok('process.environment.filename = "two/new.txt"')
    ok('os.fs.copy(source:process.environment.filename, destination:process.streams.output)', b'payload\n')
    ok('write "child.sh"\nprocess.environment.build.label = "child"\npath.remove "/bin"\necho process.environment.build.label\npwd\n.')
    ok('sh "child.sh"', b'child\n')
    ok('echo process.environment.build.label', b'parent\n')
    ok('write "custom-profile"\nprocess.shell.aliases.add(name:"say", target:"out.print")\n.')
    ok('write "custom-grandchild"\nsay "inherited profile"\n.')
    ok('write "custom-child"\n/bin/sh "custom-grandchild"\n.')
    ok('/bin/sh init_script: "/tmp/custom-profile" < "custom-child"', b'inherited profile\n')
    ok('path.list', b'/bin\n')
    ok('write "strict.sh"\necho missing.value\necho "must-not-run"\n.')
    failed = run('sh "strict.sh"', allow_errors=True)
    assert b'unresolved expression' in failed and b'must-not-run\n' not in failed, failed
    ok('write "quiet.sh"\nfs.cwd\necho "explicit"\n.')
    ok('sh "quiet.sh" > "quiet.out"')
    result = ok('cat "quiet.out"', b'explicit\n')
    assert b'/tmp\n' not in result, result
    failed = run('fs.copy source:"two/new.txt" "other"', allow_errors=True)
    assert b'duplicate parameter' in failed, failed
    failed = run('cp "two/new.txt" "two/new.txt"', allow_errors=True)
    assert b'failed' in failed, failed
    failed = run('fs.copy process.streams.output destination:"two/new.txt"', allow_errors=True)
    assert b'stream lacks required access' in failed, failed
    failed = run('cat "two/new.txt" > "two/new.txt"', allow_errors=True)
    assert b'redirection failed' in failed, failed
    failed = run('mv "two/new.txt/" "lost"', allow_errors=True)
    assert b'failed' in failed, failed
    failed = run('rm "two/new.txt/"', allow_errors=True)
    assert b'failed' in failed, failed
    ok('cat "two/new.txt"', b'payload\n')
    ok('write "here.sh"\ncat << END > "/tmp/from-script"\nheredoc body\nEND\ncat "/tmp/from-script"\n.')
    ok('sh "here.sh"', b'heredoc body\n')
    ok('write "bad-argv.sh"\n/bin/cat missing:name > "/tmp/guard"\necho "must-not-run"\n.')
    ok('echo "preserved" > "/tmp/guard"')
    failed = run('sh "bad-argv.sh"', allow_errors=True)
    assert b'invalid executable argument' in failed and b'must-not-run\n' not in failed, failed
    ok('cat "/tmp/guard"', b'preserved\n')
    failed = run('paramdemo mode:bogus > "/tmp/guard"', allow_errors=True)
    assert b'unknown enum name' in failed, failed
    ok('cat "/tmp/guard"', b'preserved\n')
    ok('echo "colon file" > "/tmp/with:colon"')
    ok('/bin/cat "/tmp/with:colon"', b'colon file\n')
    ok('cd "/"')
    ok('mkdir "move-a"')
    ok('mkdir "move-b"')
    ok('echo "persistent move" > "move-a/source"')
    ok('echo "old destination" > "move-b/target"')
    ok('mv "move-a/source" "move-b/target"')
    ok('cat "move-b/target"', b'persistent move\n')
    ok('mkdir "move-a/sub"')
    ok('cd "move-a/sub"')
    ok('mv "/move-a/sub" "/move-b/sub"')
    ok('pwd', b'/move-b/sub\n')
    ok('/diskutil sync', b'sync ok')
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
    boot(args, disk, 'first', exercise=exercise)
    boot(args, disk, 'reboot', commands=[
        ('cat "/move-b/target"', b'persistent move\n'),
        ('ls "/move-a"', None),
        ('cd "/move-b/sub"', None),
        ('pwd', b'/move-b/sub\n'),
    ])
    print('typed shell, child isolation, and move reboot checks passed')


if __name__ == '__main__':
    main()
