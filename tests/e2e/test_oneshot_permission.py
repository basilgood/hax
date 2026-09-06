#!/usr/bin/env python3
"""Headless runs keep hax's original behavior: outside-workspace access is not gated."""

import harness

# The transcript lands in the scratch workdir (the process cwd).
result = harness.run_oneshot(
    "go", "permission.txt", extra_env={"HAX_TRANSCRIPT": "transcript.txt"}
)
harness.expect(result.returncode == 0, "exit status is 0", result)

outside = result.workdir.parent / "outside.txt"
harness.expect(outside.exists(), "headless outside write runs ungated (yolo)", result)
harness.expect(outside.read_text() == "pwned\n", "outside.txt has the scripted content", result)

inside = result.workdir / "inside.txt"
harness.expect(inside.exists(), "inside write ran", result)
harness.expect(inside.read_text() == "ok\n", "inside.txt has the scripted content", result)

transcript = result.workdir / "transcript.txt"
harness.expect(transcript.exists(), "transcript was written", result)
harness.expect(
    "permission denied" not in transcript.read_text(),
    "no denial reaches the model headlessly",
    result,
)
harness.expect("All done." in result.stdout, "final assistant text reaches stdout", result)
