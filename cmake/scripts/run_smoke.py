#!/usr/bin/env python3
"""Run one QEMU smoke, capture serial output, and write structured evidence."""

import argparse
from collections import deque
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import threading
import time


ANSI_ESCAPE_RE = re.compile(r"\x1b(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")
TEST_NAME_RE = re.compile(r"[A-Za-z0-9_.-]{1,128}")
MAX_MATCH_BUFFER_CHARS = 1024 * 1024
MAX_ERROR_TAIL_CHARS = 512 * 1024
MAX_MARKERS = 256
MAX_EVENTS = 128
MAX_VALUE_CHARS = 16 * 1024
MAX_PATH_CHARS = 4096
MAX_QEMU_ARGUMENTS = 1024
MAX_TIMEOUT_SECONDS = 600.0
MAX_SETTLE_SECONDS = 60.0
MAX_SEND_DELAY_SECONDS = 10.0
SHELL_PROMPT = "os1> "
QEMU_ENV_UNSET_VARS = (
    "LD_LIBRARY_PATH",
    "LD_PRELOAD",
    "GIO_MODULE_DIR",
    "GTK_EXE_PREFIX",
    "GTK_IM_MODULE_FILE",
    "GTK_MODULES",
    "GTK_PATH",
    "XDG_DATA_DIRS",
    "XDG_DATA_HOME",
)


@dataclass
class SendEvent:
    marker: str
    text: str
    sent: bool = False


@dataclass
class MonitorEvent:
    marker: str
    text: str
    sent: bool = False


@dataclass
class SmokeOutcome:
    status: str
    reason: str
    message: str
    elapsed_seconds: float
    seen: list[str]
    missing: list[str]
    rejected: list[str]
    output_tail: list[str]


class OutputTail:
    def __init__(self, max_chars=MAX_ERROR_TAIL_CHARS):
        self.max_chars = max_chars
        self.lines = deque()
        self.char_count = 0

    def append(self, line):
        self.lines.append(line)
        self.char_count += len(line)
        while self.char_count > self.max_chars and self.lines:
            self.char_count -= len(self.lines.popleft())

    def snapshot(self):
        return list(self.lines)


def normalize_for_marker_matching(text, sent_serial_texts, marker_noise=()):
    # The guest shell prompt and echoed input share the same serial stream as
    # kernel/user output, so they can appear in the middle of marker text.
    normalized = ANSI_ESCAPE_RE.sub("", text).replace("\r", "")
    normalized = normalized.replace(SHELL_PROMPT, "")
    for sent_text in sent_serial_texts:
        if sent_text:
            normalized = normalized.replace(sent_text, "")
    for marker in marker_noise:
        if marker:
            normalized = normalized.replace(marker, "")
    return normalized


def compact_for_marker_matching(text):
    return "".join(text.split())


def contains_control_character(value):
    return any(ord(character) < 32 or ord(character) == 127 for character in value)


def validate_values(parser, values, flag_name, max_count):
    if len(values) > max_count:
        parser.error(f"{flag_name} may be provided at most {max_count} times")
    if len(values) != len(set(values)):
        parser.error(f"{flag_name} values must be unique")
    for value in values:
        if not value:
            parser.error(f"{flag_name} values must not be empty")
        if len(value) > MAX_VALUE_CHARS:
            parser.error(f"{flag_name} values must not exceed {MAX_VALUE_CHARS} characters")
        if contains_control_character(value):
            parser.error(f"{flag_name} values must not contain control characters")


def parse_event_spec(parser, spec, flag_name):
    if len(spec) > MAX_VALUE_CHARS:
        parser.error(f"{flag_name} values must not exceed {MAX_VALUE_CHARS} characters")
    marker, separator, encoded_text = spec.partition("::")
    if not separator or not marker:
        parser.error(f"invalid {flag_name} value, expected non-empty MARKER::TEXT")
    if contains_control_character(marker):
        parser.error(f"{flag_name} markers must not contain control characters")
    try:
        text = bytes(encoded_text, "utf-8").decode("unicode_escape")
    except UnicodeDecodeError:
        parser.error(f"invalid escape sequence in {flag_name} value")
    if len(marker) > MAX_VALUE_CHARS or len(text) > MAX_VALUE_CHARS:
        parser.error(f"{flag_name} marker/text must not exceed {MAX_VALUE_CHARS} characters")
    return marker, text


def validate_path(parser, value, flag_name):
    if not value or len(value) > MAX_PATH_CHARS:
        parser.error(f"{flag_name} must be a non-empty path no longer than {MAX_PATH_CHARS} characters")
    if contains_control_character(value):
        parser.error(f"{flag_name} must not contain control characters")


def validate_duration(parser, value, flag_name, minimum, maximum):
    if not math.isfinite(value) or value < minimum or value > maximum:
        parser.error(f"{flag_name} must be between {minimum:g} and {maximum:g} seconds")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, help="Path to write captured serial output.")
    parser.add_argument("--summary", required=True, help="Path to write the JSON smoke summary.")
    parser.add_argument("--test-name", required=True, help="Stable CTest smoke-test name.")
    parser.add_argument("--boot-path", required=True, choices=("uefi", "bios"),
                        help="Boot path exercised by this smoke.")
    parser.add_argument("--timeout", type=float, required=True, help="Wall-clock ceiling in seconds.")
    parser.add_argument("--settle-after-markers", type=float, default=0.0,
                        help="Extra time to keep streaming after all required markers are seen.")
    parser.add_argument("--send-delay", type=float, default=0.0,
                        help="Delay before writing a matched --send-after payload.")
    parser.add_argument("--marker", action="append", default=[],
                        help="Required marker substring (repeatable, order-independent).")
    parser.add_argument("--reject-marker", action="append", default=[],
                        help="Forbidden marker substring (repeatable, fails immediately if seen).")
    parser.add_argument("--send-after", action="append", default=[],
                        help="Send text to QEMU stdin after MARKER using MARKER::TEXT with \\n escapes.")
    parser.add_argument("--monitor-socket", default=None,
                        help="Optional UNIX socket path for a QEMU HMP monitor.")
    parser.add_argument("--monitor-send-after", action="append", default=[],
                        help="Send HMP text after MARKER using MARKER::TEXT with \\n escapes.")
    parser.add_argument("qemu_cmd", nargs=argparse.REMAINDER,
                        help="Separator '--' followed by the QEMU command to run.")
    args = parser.parse_args(argv)

    cmd = list(args.qemu_cmd)
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        parser.error("missing QEMU command after '--'")

    validate_path(parser, args.log, "--log")
    validate_path(parser, args.summary, "--summary")
    if Path(args.log).resolve() == Path(args.summary).resolve():
        parser.error("--log and --summary must identify different files")
    if TEST_NAME_RE.fullmatch(args.test_name) is None:
        parser.error("--test-name must contain 1-128 letters, digits, '.', '_', or '-'")
    validate_duration(parser, args.timeout, "--timeout", 0.1, MAX_TIMEOUT_SECONDS)
    validate_duration(parser, args.settle_after_markers, "--settle-after-markers", 0.0,
                      MAX_SETTLE_SECONDS)
    validate_duration(parser, args.send_delay, "--send-delay", 0.0, MAX_SEND_DELAY_SECONDS)
    validate_values(parser, args.marker, "--marker", MAX_MARKERS)
    validate_values(parser, args.reject_marker, "--reject-marker", MAX_MARKERS)
    if not args.marker:
        parser.error("at least one --marker is required")
    if set(args.marker) & set(args.reject_marker):
        parser.error("the same value cannot be both a required and forbidden marker")
    if len(args.send_after) > MAX_EVENTS or len(args.monitor_send_after) > MAX_EVENTS:
        parser.error(f"send-event flags may be provided at most {MAX_EVENTS} times each")
    if len(cmd) > MAX_QEMU_ARGUMENTS:
        parser.error(f"QEMU command may contain at most {MAX_QEMU_ARGUMENTS} arguments")
    for argument in cmd:
        if not argument or len(argument) > MAX_VALUE_CHARS:
            parser.error(f"QEMU arguments must be non-empty and at most {MAX_VALUE_CHARS} characters")
        if contains_control_character(argument):
            parser.error("QEMU arguments must not contain control characters")
    if args.monitor_socket is not None:
        validate_path(parser, args.monitor_socket, "--monitor-socket")
    elif args.monitor_send_after:
        parser.error("--monitor-send-after requires --monitor-socket")

    send_events = []
    for spec in args.send_after:
        marker, text = parse_event_spec(parser, spec, "--send-after")
        send_events.append(SendEvent(marker=marker, text=text))
    monitor_events = []
    for spec in args.monitor_send_after:
        marker, text = parse_event_spec(parser, spec, "--monitor-send-after")
        monitor_events.append(MonitorEvent(marker=marker, text=text))
    return args, cmd, send_events, monitor_events


def read_monitor_prompt(sock, timeout=5.0):
    # HMP echoes input a character at a time. A recv() can contain only that
    # echo; the next prompt is the acknowledgement that the command finished.
    deadline = time.monotonic() + timeout
    response = b""
    while not response.endswith(b"(qemu) "):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("timed out waiting for QEMU monitor prompt")
        sock.settimeout(remaining)
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("QEMU monitor closed before its prompt")
        response = (response + chunk)[-4096:]


def send_monitor_text(socket_path, text):
    commands = [line for line in text.splitlines() if line]
    if not commands:
        commands = [text]

    for command in commands:
        deadline = time.monotonic() + 5.0
        last_error = None
        payload = command if command.endswith("\n") else (command + "\n")
        while time.monotonic() < deadline:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.connect(socket_path)
                break
            except OSError as exc:
                sock.close()
                last_error = exc
                time.sleep(0.05)
        else:
            raise RuntimeError(f"failed to connect to monitor '{socket_path}': {last_error}")

        # Retry connection establishment only. Once sent, a command must not
        # be replayed if the acknowledgement is lost (e.g. duplicate keys).
        try:
            with sock:
                read_monitor_prompt(sock)
                sock.sendall(payload.encode("utf-8"))
                read_monitor_prompt(sock)
        except OSError as exc:
            raise RuntimeError(f"monitor command {command!r} failed: {exc}") from exc

        time.sleep(0.05)


def terminate(proc):
    if proc.poll() is None:
        try:
            proc.terminate()
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
        except ProcessLookupError:
            pass
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except (ProcessLookupError, OSError):
        pass


def scrub_qemu_env():
    env = os.environ.copy()
    for name in QEMU_ENV_UNSET_VARS:
        env.pop(name, None)
    return env


def normalize_path_argument(value):
    if not os.path.isabs(value):
        return value
    name = Path(value).name
    return f"<abs>/{name}" if name else "<abs>"


def normalize_qemu_argument(argument):
    if os.path.isabs(argument):
        return normalize_path_argument(argument)
    components = argument.split(",")
    normalized = []
    for component in components:
        key, separator, value = component.partition("=")
        if separator and key == "file":
            normalized.append(f"{key}={normalize_path_argument(value)}")
        elif component.startswith("unix:"):
            normalized.append(f"unix:{normalize_path_argument(component[5:])}")
        else:
            normalized.append(component)
    return ",".join(normalized)


def qemu_version(command, env):
    try:
        result = subprocess.run(
            [command, "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            text=True,
            errors="replace",
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"unavailable: {type(exc).__name__}"
    lines = result.stdout.splitlines()
    if result.returncode != 0:
        return f"unavailable: exit {result.returncode}"
    if not lines:
        return "unavailable: empty version output"
    return lines[0][:512]


def summary_document(args, cmd, version, outcome):
    return {
        "schema_version": 1,
        "test_name": args.test_name,
        "boot_path": args.boot_path,
        "result": {
            "status": outcome.status,
            "reason": outcome.reason,
            "message": outcome.message,
        },
        "timing": {
            "elapsed_seconds": round(outcome.elapsed_seconds, 3),
            "timeout_seconds": args.timeout,
            "settle_after_markers_seconds": args.settle_after_markers,
        },
        "markers": {
            "expected": list(args.marker),
            "seen": outcome.seen,
            "missing": outcome.missing,
            "forbidden": list(args.reject_marker),
            "rejected": outcome.rejected,
        },
        "artifacts": {
            "serial_log": str(Path(args.log).resolve()),
            "summary": str(Path(args.summary).resolve()),
        },
        "qemu": {
            "version": version,
            "executable": Path(cmd[0]).name,
            "normalized_arguments": [normalize_qemu_argument(argument) for argument in cmd[1:]],
        },
    }


def write_summary(path, document):
    destination = Path(path)
    temporary = destination.with_name(f".{destination.name}.{os.getpid()}.tmp")
    try:
        with temporary.open("w", encoding="utf-8") as summary_file:
            json.dump(document, summary_file, indent=2, sort_keys=True)
            summary_file.write("\n")
        os.replace(temporary, destination)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def make_outcome(status, reason, message, started, markers, seen=(), rejected=(), output_tail=()):
    seen_set = set(seen)
    return SmokeOutcome(
        status=status,
        reason=reason,
        message=message,
        elapsed_seconds=max(0.0, time.monotonic() - started),
        seen=[marker for marker in markers if marker in seen_set],
        missing=[marker for marker in markers if marker not in seen_set],
        rejected=list(rejected),
        output_tail=list(output_tail),
    )


def execute_smoke(args, cmd, send_events, monitor_events):
    markers = list(args.marker)
    reject_markers = list(args.reject_marker)
    seen = set()
    output_tail = OutputTail()
    match_buffer = ""
    done = threading.Event()
    started = time.monotonic()
    deadline = started + args.timeout
    timed_out = False
    seen_at_timeout = None
    settle_completed = False
    rejected_ref = [None]
    monitor_error_ref = [None]
    reader_error_ref = [None]
    settle_deadline_ref = [None]

    try:
        log_file = open(args.log, "w", encoding="utf-8", errors="replace")
    except OSError as exc:
        return make_outcome(
            "failed", "log_open_failed", f"could not open serial log: {exc}", started, markers
        )

    with log_file:
        try:
            proc = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                bufsize=1,
                env=scrub_qemu_env(),
                text=True,
                errors="replace",
                start_new_session=True,
            )
        except OSError as exc:
            return make_outcome(
                "failed", "qemu_start_failed", f"could not start QEMU: {exc}", started, markers
            )

        def reader():
            nonlocal match_buffer
            try:
                assert proc.stdout is not None
                for line in proc.stdout:
                    output_tail.append(line)
                    log_file.write(line)
                    log_file.flush()
                    match_buffer = (match_buffer + line)[-MAX_MATCH_BUFFER_CHARS:]
                    sent_serial_texts = [event.text for event in send_events if event.sent]
                    marker_noise = [marker for marker in markers if marker in match_buffer]
                    normalized_line = normalize_for_marker_matching(line, sent_serial_texts)
                    normalized_match_buffer = normalize_for_marker_matching(
                        match_buffer, sent_serial_texts, marker_noise
                    )
                    compact_buffer = compact_for_marker_matching(normalized_match_buffer)
                    for marker in reject_markers:
                        if marker in line or marker in normalized_line:
                            rejected_ref[0] = marker
                            done.set()
                            return
                    for marker in markers:
                        if marker not in seen and (
                            marker in line
                            or marker in match_buffer
                            or marker in normalized_match_buffer
                            or compact_for_marker_matching(marker) in compact_buffer
                        ):
                            seen.add(marker)
                    if proc.stdin is not None:
                        for event in send_events:
                            if not event.sent and (
                                event.marker in seen
                                or event.marker in line
                                or event.marker in match_buffer
                                or event.marker in normalized_match_buffer
                            ):
                                try:
                                    if args.send_delay > 0.0:
                                        time.sleep(args.send_delay)
                                    proc.stdin.write(event.text)
                                    proc.stdin.flush()
                                except BrokenPipeError:
                                    pass
                                event.sent = True
                    if args.monitor_socket is not None:
                        for event in monitor_events:
                            if not event.sent and (
                                event.marker in seen
                                or event.marker in line
                                or event.marker in match_buffer
                                or event.marker in normalized_match_buffer
                            ):
                                try:
                                    send_monitor_text(args.monitor_socket, event.text)
                                except RuntimeError as exc:
                                    monitor_error_ref[0] = str(exc)
                                    done.set()
                                    return
                                event.sent = True
                    if len(seen) == len(markers):
                        if args.settle_after_markers <= 0:
                            done.set()
                            return
                        if settle_deadline_ref[0] is None:
                            settle_deadline_ref[0] = time.monotonic() + args.settle_after_markers
                        if time.monotonic() >= settle_deadline_ref[0]:
                            done.set()
                            return
            except Exception as exc:  # Report thread failures through the structured result.
                reader_error_ref[0] = f"{type(exc).__name__}: {exc}"
            finally:
                done.set()

        reader_thread = threading.Thread(target=reader, daemon=True)
        reader_thread.start()

        while not done.is_set():
            current_deadline = settle_deadline_ref[0] or deadline
            remaining = current_deadline - time.monotonic()
            if remaining <= 0:
                if settle_deadline_ref[0] is None:
                    timed_out = True
                    seen_at_timeout = set(seen)
                else:
                    settle_completed = True
                break
            done.wait(timeout=min(0.2, remaining))

        if settle_deadline_ref[0] is not None and time.monotonic() >= settle_deadline_ref[0]:
            settle_completed = True
        terminate(proc)
        reader_thread.join(timeout=2)
        if reader_thread.is_alive() and reader_error_ref[0] is None:
            reader_error_ref[0] = "serial reader did not stop after QEMU termination"

    tail = output_tail.snapshot()
    if timed_out:
        deadline_seen = seen_at_timeout or set()
        message = f"matched {len(deadline_seen)}/{len(markers)} required markers before timeout"
        return make_outcome(
            "failed", "timeout", message, started, markers, deadline_seen, output_tail=tail
        )
    if rejected_ref[0] is not None:
        marker = rejected_ref[0]
        return make_outcome(
            "failed", "forbidden_marker_seen", f"forbidden marker seen: {marker}", started,
            markers, seen, (marker,), tail
        )
    if monitor_error_ref[0] is not None:
        return make_outcome(
            "failed", "monitor_command_failed", monitor_error_ref[0], started, markers,
            seen, output_tail=tail
        )
    if reader_error_ref[0] is not None:
        return make_outcome(
            "failed", "serial_reader_failed", reader_error_ref[0], started, markers,
            seen, output_tail=tail
        )

    missing = [marker for marker in markers if marker not in seen]
    if missing:
        message = f"matched {len(seen)}/{len(markers)} required markers"
        return make_outcome(
            "failed", "qemu_exited_before_markers", message, started, markers, seen,
            output_tail=tail
        )
    if args.settle_after_markers > 0.0 and not settle_completed:
        return make_outcome(
            "failed", "qemu_exited_during_settle",
            "QEMU exited before the clean settle interval completed", started, markers,
            seen, output_tail=tail
        )
    message = f"matched all {len(markers)} required markers"
    if args.settle_after_markers > 0.0:
        message += f" and stayed clean for {args.settle_after_markers:.2f}s"
    return make_outcome("passed", "all_markers_seen", message, started, markers, seen,
                        output_tail=tail)


def report_outcome(args, outcome):
    destination = str(Path(args.summary).resolve())
    log_path = str(Path(args.log).resolve())
    stream = sys.stdout if outcome.status == "passed" else sys.stderr
    stream.write(
        f"Smoke run {outcome.status} [{outcome.reason}] in {outcome.elapsed_seconds:.2f}s: "
        f"{outcome.message}.\n"
    )
    if outcome.missing:
        stream.write("Missing markers:\n")
        for marker in outcome.missing:
            stream.write(f"  - {marker}\n")
    if outcome.rejected:
        stream.write("Rejected markers:\n")
        for marker in outcome.rejected:
            stream.write(f"  - {marker}\n")
    stream.write(f"Serial log: {log_path}\nJSON summary: {destination}\n")
    if outcome.status == "failed":
        stream.write("--- captured serial log tail ---\n")
        if outcome.output_tail:
            stream.writelines(outcome.output_tail)
            if not outcome.output_tail[-1].endswith("\n"):
                stream.write("\n")
        else:
            stream.write("(empty)\n")
        stream.write("--- end serial log tail ---\n")


def main(argv=None):
    args, cmd, send_events, monitor_events = parse_args(argv)
    env = scrub_qemu_env()
    version = qemu_version(cmd[0], env)
    started = time.monotonic()
    initial = make_outcome(
        "running", "runner_started", "QEMU smoke runner started", started, args.marker
    )
    try:
        write_summary(args.summary, summary_document(args, cmd, version, initial))
    except OSError as exc:
        sys.stderr.write(f"Smoke runner could not initialize JSON summary '{args.summary}': {exc}\n")
        return 2

    try:
        outcome = execute_smoke(args, cmd, send_events, monitor_events)
    except Exception as exc:  # Preserve a structured failure for unexpected runner defects.
        outcome = make_outcome(
            "failed", "runner_internal_error", f"{type(exc).__name__}: {exc}", started,
            args.marker
        )

    try:
        write_summary(args.summary, summary_document(args, cmd, version, outcome))
    except OSError as exc:
        sys.stderr.write(f"Smoke runner could not finalize JSON summary '{args.summary}': {exc}\n")
        return 1

    report_outcome(args, outcome)
    return 0 if outcome.status == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
