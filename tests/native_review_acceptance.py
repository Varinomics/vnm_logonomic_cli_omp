"""Opt-in actual OMP read-only RPC gate with an isolated local model.

The SDK host driver invokes the packaged adapter. This harness then performs
the host's documented managed-asset substitution and owns the native process.
It does not claim to run the desktop process custodian or an OS sandbox.
"""

import argparse
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import subprocess
import threading
import time

import native_acceptance as native


class ReviewModel:
    def __init__(self, delegated):
        self.requests = []
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_arguments):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                fixture.requests.append({"path": self.path, "body": body})
                native.require(self.path == "/v1/chat/completions", "Unexpected model route")
                index = len(fixture.requests) - 1
                native.require(index < 12, "The native review did not settle after bounded tool refusals")
                plan = [
                    ("read", {"path": "review.txt"}),
                    ("write", {"path": "review.txt", "content": "forbidden replacement"}),
                    ("bash", {"command": "printf forbidden > forbidden.txt"}),
                    ("read", {"path": "ssh://fixture.invalid/forbidden"}),
                    ("task", {"task": "Write forbidden.txt"}),
                ]
                if delegated:
                    results = json.dumps([message for message in body["messages"] if message.get("role") == "tool"])
                    names = re.findall(r"xd://([a-z0-9_]+_report_result)\b", results)
                    name = names[0] if names else "missing"
                    plan.extend([
                        ("read", {"path": "xd://"}),
                        ("write", {"path": "xd://" + name,
                            "content": json.dumps({"message": "native task result"})}),
                        ("write", {"path": "xd://" + name.removesuffix("report_result") + "send_message",
                            "content": json.dumps({"message": "forbidden action"})}),
                    ])
                if index < len(plan):
                    name, arguments = plan[index]
                    if delegated and index in (6, 7):
                        native.require(name == "write" and "missing" not in arguments["path"],
                            "Native xd inventory did not expose the exact authorized host action")
                    delta = {"role": "assistant", "tool_calls": [{"index": 0, "id": f"review-call-{index}",
                        "type": "function", "function": {"name": name, "arguments": json.dumps(arguments)}}]}
                    finish = "tool_calls"
                else:
                    delta, finish = {"role": "assistant", "content": native.ANSWER}, "stop"
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                for value, reason in ((delta, None), ({}, finish)):
                    frame = {"id": "review-completion", "object": "chat.completion.chunk", "created": 1,
                        "model": "model", "choices": [{"index": 0, "delta": value, "finish_reason": reason}]}
                    self.wfile.write(("data: " + json.dumps(frame) + "\n\n").encode())
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class ReviewCase(native.Native_case):
    def __init__(self, arguments, delegated):
        super().__init__(arguments, "review-delegated" if delegated else "review-structured")
        self.delegated = delegated
        self.model.close()
        self.model = ReviewModel(delegated)
        configuration = self.root / "agent/models.yml"
        models = json.loads(configuration.read_text())
        models["providers"]["logonomic-fixture"]["baseUrl"] = f"http://127.0.0.1:{self.model.server.server_port}/v1"
        native.write_json(configuration, models)
        (self.root / "workspace/review.txt").write_text("READ_ONLY_SOURCE_EVIDENCE\n")

    def host(self, capability, version, operation, payload, name):
        result = subprocess.run([str(self.arguments.host_driver), str(self.arguments.package)],
            input=json.dumps({"capability_id": capability, "version": version, "operation": operation, "payload": payload}),
            capture_output=True, text=True, timeout=20)
        native.require(result.returncode == 0, f"SDK typed host failed: {result.stderr}")
        outcome = json.loads(result.stdout)
        native.write_json(self.root / f"host-{name}.json", {"request": payload, "outcome": outcome})
        native.require(outcome.get("status") == "success", f"Actual adapter refused {name}: {outcome}")
        return outcome["payload"]

    def inspect(self, state, output=b"", error=b"", final=False, exit_code=0):
        request = {"purpose": "read_only_review", "failure_diagnostics": {"version": 1}, "inspection": {
            "protocol_state_base64_chunks": state, "stdout_base64_chunks": native.chunks(output),
            "stderr_base64_chunks": native.chunks(error), "final": final, "timed_out": False, "exit_code": exit_code}}
        result = subprocess.run([str(self.arguments.driver), "--inspect"], input=json.dumps(request),
            capture_output=True, text=True, timeout=5)
        native.require(result.returncode == 0, f"Production parser driver failed: {result.stderr}")
        response = json.loads(result.stdout)
        self.transcript.append({"request": request, "result": response})
        return response, request

    def run_review(self):
        process = None
        frames, stderr = [], []
        try:
            capability = "delegated-session" if self.delegated else "structured-session"
            request = self.prepare_request(self.delegated)
            request["purpose"] = "read_only_review"
            request["declared_assets"] = json.loads((self.arguments.package /
                "capabilities/managed-assets@1.json").read_text())["payload"]["assets"]
            if self.delegated:
                # The actual relay advertises two actions; this task authorizes
                # one. Its other action must not become review authority.
                request["control"]["servers"][0]["actions"] = ["report_result"]
            prepared = self.host(capability, 3, "prepare", request, "prepare")
            native.write_json(self.root / "prepared.json", prepared)
            for artifact in prepared.get("private_artifacts", []):
                target = self.root / "plugin" / artifact["relative_path"]
                native.require(target.resolve().is_relative_to((self.root / "plugin").resolve()), "Artifact path escaped")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(base64.b64decode(artifact["content_base64"], validate=True))
            proposal = prepared["process"]
            environment = {name: self.environment[name] for name in proposal["environment_names"]}
            copied = self.root / "materialized/assets/native_review.ts"
            copied.parent.mkdir(parents=True)
            shutil.copyfile(self.arguments.package / "assets/native_review.ts", copied)
            for name, value in proposal["environment"].items():
                environment[name] = value.replace("{{asset:assets/native_review.ts}}", str(copied))
                native.require("{{asset:" not in environment[name], "Unknown managed asset reference")
            command = [str(self.arguments.package / proposal["executable"]), *proposal["argv"]]
            process = subprocess.Popen(command, cwd=self.root / "workspace", env=environment,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            events = queue.Queue()

            def read(stream, name):
                for line in iter(stream.readline, b""):
                    events.put((name, line))
                events.put((name, None))

            for stream, name in ((process.stdout, "stdout"), (process.stderr, "stderr")):
                threading.Thread(target=read, args=(stream, name), daemon=True).start()
            state = prepared["inspection"]["protocol_state_base64_chunks"]
            deadline = time.monotonic() + 60
            closed = set()
            while len(closed) < 2:
                native.require(time.monotonic() < deadline, "Native review exceeded its acceptance deadline")
                name, data = events.get(timeout=max(0.01, deadline - time.monotonic()))
                if data is None:
                    closed.add(name)
                    continue
                (frames if name == "stdout" else stderr).append(json.loads(data) if name == "stdout" else data.decode(errors="replace"))
                result, _request = self.inspect(state, data if name == "stdout" else b"", data if name == "stderr" else b"")
                native.require(not result.get("failure_reason"), f"Actual native review failed: {result}")
                native.require(not result["complete"], "Native review completed before process exit")
                state = result["protocol_state_base64_chunks"]
                pending = native.unchunks(result.get("stdin_base64_chunks", []))
                if pending:
                    process.stdin.write(pending)
                    process.stdin.flush()
                if result.get("close_stdin"):
                    process.stdin.close()
            exit_code = process.wait(timeout=5)
            result, final = self.inspect(state, final=True, exit_code=exit_code)
            native.require(exit_code == 0 and result["complete"] and not result.get("failure_reason"),
                f"Native review exit was not successful: {result}")
            native.require(native.unchunks(result["result_base64_chunks"]).decode() == native.ANSWER, "Answer changed")
            native.require(self.host("structured-session", 3, "inspect", final, "final") == result, "Actual handler diverged")
            native.require((self.root / "workspace/review.txt").read_text() == "READ_ONLY_SOURCE_EVIDENCE\n" and
                not (self.root / "workspace/forbidden.txt").exists(), "Read-only native tools changed workspace content")
            tool_results = [frame["message"] for frame in frames if frame.get("type") == "message_end" and
                frame.get("message", {}).get("role") == "toolResult"]
            native.require("READ_ONLY_SOURCE_EVIDENCE" in json.dumps(tool_results), "Native file read never executed")
            native.require(sum(bool(row.get("isError")) for row in tool_results) >= 4,
                "Native mutating/remote tool attempts were not refused")
            if self.delegated:
                relay = [json.loads(line) for line in (self.root / "relay.jsonl").read_text().splitlines()]
                calls = [row["params"]["name"] for row in relay if row.get("method") == "tools/call"]
                native.require(calls == ["report_result"], "Native review exceeded the exact host action authority")
            return {"scenario": self.name, "exit_code": exit_code, "model_requests": len(self.model.requests),
                "read_succeeded": True, "workspace_unchanged": True,
                "refused_tool_results": sum(bool(row.get("isError")) for row in tool_results),
                "task_control_calls": calls if self.delegated else []}
        finally:
            if process is not None and process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            self.model.close()
            native.write_json(self.root / "native-frames.json", frames)
            (self.root / "native-stderr.txt").write_text("".join(stderr))
            native.write_json(self.root / "model-requests.json", self.model.requests)
            native.write_json(self.root / "inspection-transcript.json", self.transcript)


def main():
    parser = argparse.ArgumentParser()
    for name in ("runtime", "package", "driver", "host-driver", "artifacts"):
        parser.add_argument("--" + name, type=Path, required=True)
    arguments = parser.parse_args()
    for name in ("runtime", "package", "driver", "host_driver", "artifacts"):
        setattr(arguments, name, getattr(arguments, name).resolve())
    arguments.artifacts.mkdir(exist_ok=False)
    files = {"harness": Path(__file__), "fixture_library": Path(native.__file__), "driver": arguments.driver,
        "host_driver": arguments.host_driver,
        "native_cli": arguments.runtime / "node_modules/@oh-my-pi/pi-coding-agent/dist/cli.js",
        "bun": arguments.runtime / "node_modules/@oven/bun-linux-x64/bin/bun"}
    files.update({"package/" + str(path.relative_to(arguments.package)): path
        for path in arguments.package.rglob("*") if path.is_file()})
    native.write_json(arguments.artifacts / "metadata.json", {"file_sha256": {
        name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in sorted(files.items())},
        "model_transport": "isolated loopback fixture; no real account", "native_session_persistence": False})
    result = [ReviewCase(arguments, delegated).run_review() for delegated in (False, True)]
    native.write_json(arguments.artifacts / "result.json", result)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
