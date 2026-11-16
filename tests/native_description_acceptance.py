"""Opt-in official OMP description-tool gate with a fixture host receipt.

The real packaged extension, native tool execution, helper and adapter classifier
run here. The vnmctl receiver is a fixture: its accepted receipt does not claim
execution of the production Session-service title/summary policy.
"""

import argparse
import fcntl
import hashlib
import http.server
import json
import os
from pathlib import Path
import pty
import shlex
import signal
import struct
import subprocess
import sys
import termios
import threading
import time

import native_acceptance as native


TOOL = "logonomic_describe_session"
REQUEST_ID = "description-fixture-call-1"
TITLE = "Native description café"
SUMMARY = "# Working state\n\nThe isolated native tool returned this deliberate model-authored summary."
RECORDED = "Logonomic recorded the session description."
INSTRUCTION = "Suggest the session title and record the concise current working summary."


def wrapper_fixture():
    descriptor = json.loads(Path(sys.argv[2]).read_text())
    arguments = sys.argv[3:]
    body = json.loads(sys.stdin.read()) if "--args-stdin" in arguments else None
    if arguments[0] == "request-title-instruction":
        result = {"accepted": True, "instruction_required": True, "instruction": INSTRUCTION}
    else:
        result = {"accepted": True, "epoch": 1}
        if arguments[:2] == ["provider-event", "deliver"]:
            event = body["event"]
            if event["name"] == "session_start":
                result["helper_response"] = {"cooperative_description_version": 1}
            elif event["name"] == "model_description":
                submission = event["body"]
                native.require(submission["description"] == {"kind": "summary", "title": TITLE, "summary": SUMMARY},
                    "The native helper changed the deliberately authored description")
                result["helper_response"] = {"cooperative_description": {"version": 1,
                    "request_id": submission["request_id"], "accepted": True, "reason": ""}}
    with Path(descriptor["log"]).open("a", encoding="utf8") as log:
        log.write(json.dumps({"argv": arguments, "body": body, "pid": os.getpid(), "response": result}) + "\n")
    print(json.dumps(result), flush=True)


class DescriptionModel:
    def __init__(self):
        self.requests = []
        self.errors = []
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_arguments):
                pass

            def do_POST(self):
                try:
                    body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    fixture.requests.append({"path": self.path, "body": body})
                    native.require(self.path == "/v1/chat/completions", "Unexpected native model route")
                    native.require(len(fixture.requests) <= 2, "Description tool did not finish within two model requests")
                    names = [tool["function"]["name"] for tool in body.get("tools", [])]
                    native.require(TOOL in names, "The public native registry did not expose the packaged description tool")
                    if len(fixture.requests) == 1:
                        native.require(INSTRUCTION in json.dumps(body) and TOOL in json.dumps(body["messages"]),
                            "The host description instruction and negotiated tool route did not reach the model")
                        delta = {"role": "assistant", "tool_calls": [{"index": 0, "id": REQUEST_ID,
                            "type": "function", "function": {"name": TOOL, "arguments": json.dumps({
                                "kind": "summary", "title": TITLE, "summary": SUMMARY})}}]}
                        finish = "tool_calls"
                    else:
                        results = [message for message in body["messages"] if message.get("role") == "tool"]
                        native.require(any(message.get("tool_call_id") == REQUEST_ID and
                            RECORDED in json.dumps(message.get("content")) for message in results),
                            "A valid typed host receipt did not return through the actual native tool result")
                        delta, finish = {"role": "assistant", "content": native.ANSWER}, "stop"
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.end_headers()
                    for value, reason in ((delta, None), ({}, finish)):
                        frame = {"id": "description-completion", "object": "chat.completion.chunk", "created": 1,
                            "model": "model", "choices": [{"index": 0, "delta": value, "finish_reason": reason}]}
                        self.wfile.write(("data: " + json.dumps(frame) + "\n\n").encode())
                    self.wfile.write(b"data: [DONE]\n\n")
                    self.wfile.flush()
                except Exception as error:
                    fixture.errors.append(str(error))
                    self.send_error(500, str(error))

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class DescriptionCase(native.Native_case):
    def __init__(self, arguments):
        super().__init__(arguments, "description")
        self.model.close()
        self.model = DescriptionModel()
        path = self.root / "agent/models.yml"
        models = json.loads(path.read_text())
        models["providers"]["logonomic-fixture"]["baseUrl"] = f"http://127.0.0.1:{self.model.server.server_port}/v1"
        native.write_json(path, models)
        settings = self.root / "agent/config.yml"
        configuration = json.loads(settings.read_text())
        configuration["startup"] = {"setupWizard": False, "checkUpdate": False, "quiet": True}
        configuration["tools"] = {"approvalMode": "write"}
        native.write_json(settings, configuration)
        self.argv.append("--no-title")
        observer = self.root / "description-observer.ts"
        journal = json.dumps(str(self.root / "raw-native-events.jsonl"))
        observer.write_text('import { appendFileSync } from "node:fs";\n' +
            'export default function(pi) {\n' +
            ' for (const name of ["session_start", "agent_start", "tool_execution_start", "tool_execution_end", "agent_end", "session_shutdown"]) {\n' +
            '  pi.on(name, (event, ctx) => { appendFileSync(' + journal +
            ', JSON.stringify({name, event, session:ctx.sessionManager.getSessionId(), agent:ctx.agent, pid:process.pid}) + "\\n"); });\n' +
            ' }\n}\n', encoding="utf8")
        self.argv.extend(["--extension", str(observer)])

    def helper_environment(self, name="wrapper", stall=False):
        environment = super().helper_environment(name, stall)
        wrapper = Path(environment["LOGONOMIC_VNMCTL_PATH"])
        descriptor = self.root / f"{name}-descriptor.json"
        wrapper.write_text(f"#!/bin/sh\nexec {shlex.quote(sys.executable)} "
            f"{shlex.quote(str(Path(__file__).resolve()))} --wrapper-fixture "
            f"{shlex.quote(str(descriptor))} \"$@\"\n")
        return environment

    def host(self, capability, version, operation, payload, name):
        result = subprocess.run([str(self.arguments.host_driver), str(self.arguments.package)],
            input=json.dumps({"capability_id": capability, "version": version, "operation": operation, "payload": payload}),
            capture_output=True, text=True, timeout=20)
        native.require(result.returncode == 0, f"Actual SDK host driver failed: {result.stderr}")
        outcome = json.loads(result.stdout)
        native.write_json(self.root / f"host-{name}.json", {"request": payload, "outcome": outcome})
        native.require(outcome.get("status") == "success", f"Actual packaged adapter refused {name}: {outcome}")
        return outcome["payload"]

    def classify_native_submission(self):
        events = self.delivered_events()
        native.write_json(self.root / "native-observations.json", events)
        native.require(events[0]["name"] == "session_start" and events[-1]["name"] == "session_shutdown" and
            not any(event["name"] == "logonomic_observation_lost" for event in events),
            "Description delivery lost its independent native lifecycle")
        start = events[0]["body"]
        ledger = {"current_epoch": 0, "current_transition": None}
        classifications = []
        for index, event in enumerate(events):
            result = self.host("helper-ingress", 2, "classify_event", {"event": event, "ledger": ledger,
                "transition_nonce": event["body"]["activation_nonce"], "cooperative_description": {"version": 1}},
                f"event-{index}")
            classifications.append(result)
            ledger = {"current_epoch": 1, "current_transition": {"kind": "start",
                "provider_session_id": start["session_id"], "nonce": start["activation_nonce"]}}
        native.write_json(self.root / "native-classifications.json", classifications)
        descriptions = [result for result in classifications if result.get("cooperative_description", {}).get("submission")]
        native.require(len(descriptions) == 1, "The actual classifier did not expose exactly one typed native submission")
        result = descriptions[0]
        native.require(result["classification"] == "ignore" and not result.get("observations"),
            "Description submission fabricated a lifecycle observation")
        native.require(result["cooperative_description"]["submission"] == {"provider_session_id": start["session_id"],
            "transition_nonce": start["activation_nonce"], "request_id": REQUEST_ID,
            "kind": "summary", "title": TITLE, "summary": SUMMARY}, "Classifier changed the native description identity or text")
        native.require(any(row.get("outcome_transition") == "succeeded" for result in classifications
            for row in result.get("observations", [])), "Description submission suppressed native completion")
        return {"native_events": len(events), "typed_submissions": len(descriptions)}

    def run_description(self):
        process = None
        master = slave = None
        output = []
        try:
            brief = self.root / "brief.md"
            brief.write_text("Exercise the explicitly requested session description tool.\n", encoding="utf8")
            _patch, command, environment = self.prepare_interactive({"version": 1,
                "text": "Return the requested Logonomic session description, then finish this isolated fixture.",
                "referenced_file": str(brief)})
            environment["TERM"] = "xterm-256color"
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 35, 120, 0, 0))
            process = subprocess.Popen(command, cwd=self.root / "workspace", env=environment,
                stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
            os.close(slave)
            slave = None

            def read_terminal():
                try:
                    while data := os.read(master, 65536):
                        output.append(data)
                except OSError:
                    pass

            reader = threading.Thread(target=read_terminal, daemon=True)
            reader.start()
            deadline = time.monotonic() + 45
            while not any(event["name"] == "agent_end" for event in self.delivered_events()):
                native.require(not self.model.errors, "; ".join(self.model.errors))
                native.require(process.poll() is None and time.monotonic() < deadline,
                    "The native description tool did not reach its terminal event")
                time.sleep(0.02)
            native.require(not self.model.errors and len(self.model.requests) == 2,
                "The real model tool-result round trip did not complete")
            os.write(master, b"\x04")
            exit_code = process.wait(timeout=8)
            reader.join(timeout=2)
            native.require(exit_code == 0, f"Native interactive shutdown exited {exit_code}")
            raw = [json.loads(line) for line in (self.root / "raw-native-events.jsonl").read_text().splitlines()]
            calls = [row for row in raw if row["name"] == "tool_execution_end" and row["event"].get("toolName") == TOOL]
            native.require(len(calls) == 1 and calls[0]["event"]["toolCallId"] == REQUEST_ID and
                calls[0]["event"]["result"]["details"] == {"accepted": True, "disposition": "accepted", "reason": ""},
                "Official native tool execution did not retain the exact typed receipt result")
            observed = self.classify_native_submission()
            return {"scenario": "cooperative-description", "native_exit_code": exit_code,
                "model_requests": len(self.model.requests), "native_tool_invocations": len(calls),
                "typed_receipt_returned_to_model": True, "host_receiver": "fixture typed policy receipt",
                "production_host_policy_executed": False, **observed}
        finally:
            if process is not None and process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            for descriptor in (master, slave):
                if descriptor is not None:
                    os.close(descriptor)
            (self.root / "terminal.bin").write_bytes(b"".join(output))
            native.write_json(self.root / "model-requests.json", self.model.requests)
            native.write_json(self.root / "model-errors.json", self.model.errors)
            self.model.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime", "package", "host-driver", "artifacts"):
        parser.add_argument("--" + name, type=Path, required=True)
    arguments = parser.parse_args()
    for name in ("runtime", "package", "host_driver", "artifacts"):
        setattr(arguments, name, getattr(arguments, name).resolve())
    arguments.artifacts.mkdir(parents=True, exist_ok=False)
    cli = arguments.runtime / "node_modules/@oh-my-pi/pi-coding-agent"
    files = {"harness": Path(__file__), "fixture_library": Path(native.__file__), "host_driver": arguments.host_driver,
        "native_cli": cli / "dist/cli.js", "bun": arguments.runtime / "node_modules/@oven/bun-linux-x64/bin/bun"}
    files.update({"package/" + str(path.relative_to(arguments.package)): path
        for path in arguments.package.rglob("*") if path.is_file()})
    native.write_json(arguments.artifacts / "metadata.json", {"native_version": json.loads((cli / "package.json").read_text())["version"],
        "file_sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in sorted(files.items())},
        "model_transport": "isolated loopback fixture", "native_session_persistence": False,
        "host_receiver": "fixture typed policy receipt, not the production Session-service owner"})
    result = DescriptionCase(arguments).run_description()
    native.write_json(arguments.artifacts / "result.json", result)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--wrapper-fixture":
        wrapper_fixture()
    else:
        main()
