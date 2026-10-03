"""Exercise the shipped stdio CLI with its credential-free smoke provider."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import tempfile


async def verify_cli(binary, home):
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith("NEOGRAPH_HARNESS_")
        and key not in ("OPENAI_API_KEY", "OPENROUTER_API_KEY")
    }
    environment.update(HOME=home, USERPROFILE=home, XDG_CONFIG_HOME=home,
                       NEOGRAPH_HARNESS_SMOKE="1")
    process = await asyncio.create_subprocess_exec(
        binary, "--executor", "provider", cwd=home, env=environment,
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE, limit=1024 * 1024)
    diagnostics = asyncio.create_task(process.stderr.read())
    sequence = 0

    async def rpc(method, params):
        nonlocal sequence
        sequence += 1
        identifier = sequence
        request = {"jsonrpc": "2.0", "id": identifier,
                   "method": method, "params": params}
        process.stdin.write((json.dumps(request) + "\n").encode())
        await process.stdin.drain()
        while True:
            line = await asyncio.wait_for(process.stdout.readline(), 20)
            if not line:
                raise RuntimeError("Harness CLI closed before replying")
            response = json.loads(line)
            if response.get("id") != identifier:
                continue
            if "error" in response:
                raise RuntimeError(response["error"])
            return response["result"]

    async def tool(name, arguments):
        result = await rpc("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            raise RuntimeError(result)
        if "structuredContent" in result:
            return result["structuredContent"]
        return json.loads(next(item["text"] for item in result["content"]
                               if item["type"] == "text"))

    try:
        await rpc("initialize", {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": "harness-cli-regression", "version": "1"}})
        process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        await process.stdin.drain()
        # The shipped private synthetic provider admits exactly 4096 input and
        # 256 output tokens per call; no hosted model facts or credentials apply.
        request = {
            "task": {"objective": "Exercise the shipped Harness lifecycle",
                     "acceptance": ["Return a structured result"]},
            "harness": {"mode": "preset", "preset": "fanout_judge"},
            "workers": [{"id": "reviewer", "instructions": "Return structured findings",
                         "tools": [], "output_schema": {
                             "type": "object", "additionalProperties": True}}],
            "tool_catalog": [],
            "budgets": {"max_steps": 20, "timeout_seconds": 15,
                        "max_parallel_workers": 1, "max_worker_retries": 0,
                        "provider_timeout_seconds": 5, "max_output_tokens": 256},
            "policy": {"read_only": True, "evidence_required": []}}
        compiled = await tool("neograph_compile", request)
        artifact = compiled["artifact_id"]
        started = await tool("neograph_start", {"artifact_id": artifact})
        for _ in range(100):
            result = await tool("neograph_get", {"run_id": started["run_id"]})
            if result.get("status") in ("completed", "failed", "cancelled", "interrupted"):
                break
            await asyncio.sleep(0.05)
        if result.get("status") != "completed" or result.get("artifact_id") != artifact:
            raise AssertionError(result)
        summary = result["result"]
        if (summary.get("valid_workers") != 1 or summary.get("failed_workers") != 0
                or summary.get("outcome") != "zero_findings"
                or summary.get("workers") != [{"worker_id": "reviewer", "status": "completed",
                                               "result": {"status": "ok", "findings": []}}]):
            raise AssertionError(summary)
        print("Harness CLI: compile -> retained start -> completed get; one valid worker")
    finally:
        process.stdin.close()
        try:
            await asyncio.wait_for(process.wait(), 10)
        except asyncio.TimeoutError:
            process.terminate()
            try:
                await asyncio.wait_for(process.wait(), 5)
            except asyncio.TimeoutError:
                process.kill()
                await process.wait()
        diagnostic = (await diagnostics).decode(errors="replace")
        if process.returncode:
            raise RuntimeError(f"Harness CLI exited {process.returncode}: {diagnostic}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=lambda value: str(Path(value).resolve()))
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="neograph-harness-cli-") as home:
        asyncio.run(verify_cli(arguments.binary, home))
