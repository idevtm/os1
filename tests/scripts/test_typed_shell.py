#!/usr/bin/env python3
"""Typed context, shell aliases, namespace moves, and persistent reboot contract."""
import argparse
import re
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
    ok('mkdir "details"')
    short = ok('ls "."', b'a.txt\n')
    assert b'modify_ns:' not in short, short
    modified = re.search(rb' modify_ns:(\d+)', ok('stat "a.txt"'))[1]
    for command in ('ls -l "."', 'll "."', 'os.fs.list(path:".", list:true)'):
        listing = ok(command, b'- bytes:8 modify_ns:' + modified + b' a.txt\n')
        assert re.search(rb'^d bytes:\d+ modify_ns:\d+ details/\n', listing, re.MULTILINE), listing
    ok('ls -l /sbin', b' modify_ns:unsupported init\n')
    assert b'modify_ns:' not in ok('ll "." list:false', b'a.txt\n')
    failed = run('os.fs.list(path:".", list:1)', allow_errors=True)
    assert b'Boolean' in failed, failed
    ok('process.shell.aliases.show(name:"ls")', b'-l -> list:true')
    ok('process.shell.aliases.show(name:"ll")', b'os.fs.list list:true')
    for alias, report, marker in (
        ('sys', 'summary', b'sys boot='),
        ('ps', 'processes', b'pid tid pstate'),
        ('cpu', 'processors', b'cpu slot apic'),
        ('pci', 'pci', b'pci bdf vendor:device'),
        ('initrd', 'initrd', b'initrd complete'),
        ('devices', 'devices', b'devices complete'),
        ('usb', 'usb', b'USB boot snapshot'),
        ('resources', 'resources', b'resources complete'),
        ('kmem', 'memory', b'kmem complete'),
        ('events', 'events', b'events complete'),
        ('irqs', 'interrupts', b'irqs complete'),
    ):
        ok(f'process.shell.aliases.show(name:"{alias}")',
           f'os.system.diagnostics.{report}'.encode())
        ok(alias, marker)
    ok('os.system.diagnostics.interrupts()', b'irqs complete')
    pid = re.search(rb'^pid: ([1-9][0-9]*)$', ok('pid'), re.MULTILINE)[1]
    ok('process.pid', b'\n' + pid + b'\n')
    ok('process.pid > "process-id"')
    ok('cat "process-id"', b'\n' + pid + b'\n')
    ok('current_pid = process.pid')
    ok('current_pid', b'\n' + pid + b'\n')
    for alias, target in (
        ('pid', 'process.pid'), ('dmesg', 'os.system.log.print'),
        ('history', 'process.shell.history.print'), ('sync', 'os.system.sync'),
        ('shutdown', 'os.system.shutdown'), ('reboot', 'os.system.reboot'),
    ):
        ok(f'help {alias}', f'{alias} -> {target}'.encode())
    ok('sync')
    ok('os.system.sync()')
    for spelling in ('system.sync', 'system.shutdown', 'system.reboot'):
        run(f'{spelling} invalid', f'unknown command: {spelling}'.encode(), allow_errors=True)
    ok('history info', b'log capacity_bytes=')
    ok('dmesg info', b'log capacity_bytes=')
    ok('process.shell.aliases.remove(name:"history")')
    run('history info', b'unknown command: history', allow_errors=True)
    ok('process.shell.history.print limit:1')
    ok('process.shell.aliases.add(name:"history", target:"process.shell.history.print")')
    ok('process.shell.aliases.add(name:"check_help", target:"os.fs.list")')
    shown = ok('help', b'check_help -> os.fs.list')
    assert b'ls -> fs.list -l -> list:true' in shown, shown
    assert b'process.pid [property] Integer' in shown, shown
    assert b'os.system.diagnostics.interrupts [method]' in shown, shown
    ok('help os.system.diagnostics', b'interrupts [method]')
    ok('help exec', b'exec image [arguments]')
    ok('help write', b'write [--append|--replace] path')
    ok('help > "help-output"')
    ok('grep "process.pid [property] Integer" "help-output"', b'\nprocess.pid [property] Integer\n')
    ok('process.shell.aliases.remove(name:"check_help")')
    assert b'check_help ->' not in ok('help'), 'help lists a removed alias'
    ok('inspect os.system.diagnostics', b'memory [method]')
    ok('os.system.diagnostics.summary()', b'sys boot=')
    ok('os.system.diagnostics.summary > "system-report"')
    ok('cat "system-report"', b'sys boot=')
    ok('write "diagnostic.sh"\nos.system.diagnostics.summary()\n.')
    run('cp "/bin/sh" "full-report"', allow_errors=True)
    ok('stat "full-report"', b'logical_bytes:65536')
    failed = run('sh "diagnostic.sh" >> "full-report"', allow_errors=True)
    assert b'diagnostic report failed' in failed and b'command failed: sh status=1' in failed, failed
    ok('rm "full-report"')
    ok('process.shell.aliases.remove(name:"sys")')
    run('sys', b'unknown command: sys', allow_errors=True)
    ok('os.system.diagnostics.summary()', b'sys boot=')
    run('call paramdemo render', b'unknown command: call', allow_errors=True)
    ok('echo "keep" > "details/child"')
    ok('process.shell.aliases.add(name:"rmdir", target:"os.fs.remove_directory", '
       'assignment:{parameter:"recursive", value:true})')
    run('rmdir "details" recursive:false', b'failed', allow_errors=True)
    ok('cat "details/child"', b'keep\n')
    ok('rmdir "details"')
    assert b'details/\n' not in ok('ls "."'), 'recursive alias default was ignored'
    ok('process.shell.aliases.add(name:"rmdir", target:"os.fs.remove_directory")')
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
