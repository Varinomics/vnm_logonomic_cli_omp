"""Export captured native SessionManager entries through the actual adapter.

The input is the before/fork/after JSON produced by the in-memory native usage
oracle. This gate materializes those records as test fixtures; it does not
claim native persisted-session writing, resume, or a workaround for FileLock.
"""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import subprocess


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    for name in ("native-entries", "package", "host-driver", "artifacts"):
        parser.add_argument("--" + name, type=Path, required=True)
    arguments = parser.parse_args()
    for name in ("native_entries", "package", "host_driver", "artifacts"):
        setattr(arguments, name, getattr(arguments, name).resolve())
    arguments.artifacts.mkdir(exist_ok=False)
    snapshots = json.loads(arguments.native_entries.read_text())
    files = {"harness": Path(__file__), "native_entries": arguments.native_entries,
        "host_driver": arguments.host_driver}
    files.update({"package/" + str(path.relative_to(arguments.package)): path
        for path in arguments.package.rglob("*") if path.is_file()})
    metadata = {"file_sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest()
        for name, path in sorted(files.items())}, "native_persistence_exercised": False,
        "source": "Actual native in-memory SessionManager capture, materialized only as isolated export fixtures"}
    (arguments.artifacts / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    results = []

    def invoke(name, source, identity, succeeds, expected_text=(), control=None):
        output = arguments.artifacts / (name + "-output")
        output.mkdir()
        payload = {"provider_session_id": identity,
            "resume_record": {"provider_session_id": identity, "session_file": str(source)},
            "working_directory": str(arguments.artifacts),
            "launch_environment": {"platform": "linux-x86_64", "effective_environment": {}},
            "output_directory": str(output), "format": "text"}
        request = {"capability_id": "conversation-export", "version": 1, "operation": "export", "payload": payload}
        before = source.read_bytes() if source.exists() else None
        process = subprocess.run([str(arguments.host_driver), str(arguments.package)], input=json.dumps(request),
            capture_output=True, text=True, timeout=20)
        require(process.returncode == 0, "SDK host invocation failed: " + process.stderr)
        outcome = json.loads(process.stdout)
        (arguments.artifacts / (name + "-request.json")).write_text(json.dumps(request, indent=2) + "\n")
        (arguments.artifacts / (name + "-outcome.json")).write_text(json.dumps(outcome, indent=2) + "\n")
        require((outcome["status"] == "success") is succeeds, f"Unexpected export outcome for {name}: {outcome}")
        if succeeds:
            exported = output / outcome["payload"]["file_name"]
            body = exported.read_text()
            require(all(value in body for value in expected_text), "Native participant text was lost")
            require(outcome["payload"]["bytes"] == exported.stat().st_size, "Export descriptor byte count changed")
            observed = (outcome["payload"], exported.read_bytes())
            if control is not None:
                require(observed == control, "The custom filename changed native branch rendering or export counts")
        else:
            require(outcome["reason_code"] and not list(output.iterdir()), "Refused source published an export")
            observed = None
        require((source.read_bytes() if source.exists() else None) == before, "Export modified the exact source")
        results.append({"case": name, "status": outcome["status"], "reason_code": outcome["reason_code"],
            "source_unchanged": True})
        return observed

    def serialize(snapshot):
        return ("".join(json.dumps(row, ensure_ascii=False) + "\n"
            for row in [snapshot["header"], *snapshot["entries"]])).encode()

    for phase in ("before", "fork", "after"):
        snapshot = snapshots[phase]
        require(snapshot["header"]["type"] == "session" and snapshot["header"]["version"] == 3,
            "Oracle must supply a native v3 session")
        expected = []
        for entry in snapshot["entries"]:
            if entry.get("type") != "message":
                continue
            content = entry["message"]["content"]
            expected.append(content if isinstance(content, str) else
                "".join(block["text"] for block in content if block["type"] == "text"))
        control = None
        for name in ("conversation.jsonl", "work", "conversation.session"):
            key = phase + "-" + name
            source = arguments.artifacts / key
            source.write_bytes(serialize(snapshot))
            observed = invoke(key, source, snapshot["header"]["id"], True, expected, control)
            if control is None:
                control = observed

    snapshot = snapshots["before"]
    identity = snapshot["header"]["id"]
    valid = serialize(snapshot)
    invalid = {
        "gzip": gzip.compress(valid, mtime=0),
        "database": b"SQLite format 3\0" + bytes(512),
        "malformed": valid + b"{broken complete record}\n",
        "foreign-id": serialize({**snapshot, "header": {**snapshot["header"], "id": "foreign"}}),
        "unknown-version": serialize({**snapshot, "header": {**snapshot["header"], "version": 4}}),
    }
    for name, body in invalid.items():
        source = arguments.artifacts / (name + ".session")
        source.write_bytes(body)
        invoke(name, source, identity, False)
    missing = arguments.artifacts / "missing"
    missing.with_suffix(".jsonl").write_bytes(valid)
    invoke("missing-exact", missing, identity, False)
    (arguments.artifacts / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps({"passed": len(results), "native_capture_exports": 9,
        "unsupported_or_wrong_sources_refused": 6, "native_persistence_exercised": False}, indent=2))


if __name__ == "__main__":
    main()
