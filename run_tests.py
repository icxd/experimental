from __future__ import annotations

import argparse
import os
import platform
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

COMPILER = "./build/rye"
DIRECTORY = "./tests/"
PROJECT_ROOT = Path(__file__).resolve().parent
SPINNER = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
BAR_WIDTH = 28


SHARED_CACHE = PROJECT_ROOT / ".test-work" / ".rye"


def test_env() -> dict[str, str]:
    SHARED_CACHE.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["RYE_CACHE_DIR"] = str(SHARED_CACHE)
    return env


@dataclass
class TestSpec:
    path: Path
    exit_code: int
    import_paths: list[str]
    defines: list[str]
    target: str | None
    compile_only: bool
    expect_error: bool


@dataclass
class TestResult:
    path: Path
    status: str  # "pass", "fail", "skip"
    message: str


def host_triple() -> str:
    machine = platform.machine().lower()
    system = platform.system().lower()
    arch = "aarch64" if machine in {"arm64", "aarch64"} else "x86_64"
    if system == "darwin":
        os_name = "macos"
    elif system == "linux":
        os_name = "linux"
    else:
        os_name = system
    return f"{os_name}-{arch}"


def parse_test(path: Path) -> TestSpec | None:
    with open(path, "r") as f:
        lines = f.readlines()
    if not lines[0].startswith("/// "):
        return None

    exit_code = int(lines[0][3:].strip())
    import_paths: list[str] = []
    defines: list[str] = []
    target = None
    compile_only = False
    expect_error = False
    for line in lines[1:]:
        line = line.strip()
        if not line.startswith("/// "):
            break
        if line.startswith("/// import-path:"):
            import_paths.append(line[len("/// import-path:") :].strip())
        elif line.startswith("/// define:"):
            defines.append(line[len("/// define:") :].strip())
        elif line.startswith("/// target:"):
            target = line[len("/// target:") :].strip()
        elif line == "/// compile-only":
            compile_only = True
        elif line == "/// check-only":
            compile_only = True
        elif line == "/// expect-error":
            expect_error = True
    return TestSpec(
        path=path,
        exit_code=exit_code,
        import_paths=import_paths,
        defines=defines,
        target=target,
        compile_only=compile_only,
        expect_error=expect_error,
    )


def warm_cache(specs: list[TestSpec]) -> None:
    for spec in specs:
        if spec.target is not None and spec.target != host_triple():
            continue
        if spec.compile_only:
            continue
        subprocess.run(
            [COMPILER, spec.path, "-O", spec.path.with_suffix("")],
            capture_output=True,
            cwd=PROJECT_ROOT,
            env=test_env(),
        )
        return


def run_batch(specs: list[TestSpec], progress: LiveProgress) -> list[TestResult]:
    cmd = [COMPILER, "test", *[str(spec.path) for spec in specs]]
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        cwd=PROJECT_ROOT,
        env=test_env(),
    )
    assert proc.stdout is not None
    results: list[TestResult] = []
    for line in proc.stdout:
        line = line.strip()
        if not line.startswith("@test "):
            continue
        parts = line.split(maxsplit=3)
        if len(parts) < 3:
            continue
        status, path = parts[1], Path(parts[2])
        if status == "PASS":
            result = TestResult(path, "pass", "")
        elif status == "SKIP":
            result = TestResult(path, "skip", "")
        else:
            detail = parts[3] if len(parts) > 3 else "failed"
            result = TestResult(
                path,
                "fail",
                f"\033[31;1mERROR:\033[0m Test `{path}` failed ({detail}).",
            )
        results.append(result)
        progress.update(result)
    proc.wait()
    return results


def run_test(spec: TestSpec) -> TestResult:
    entry = spec.path
    if spec.target is not None and spec.target != host_triple():
        return TestResult(
            entry,
            "skip",
            f"\033[33;1m SKIP:\033[0m Test `{entry}` skipped "
            f"(requires `{spec.target}`, host is `{host_triple()}`).",
        )

    cmd = [COMPILER, entry, "-O", entry.with_suffix("")]
    for import_path in spec.import_paths:
        cmd.extend(["-I", import_path])
    for define in spec.defines:
        cmd.extend(["-D", define])
    if spec.target is not None:
        cmd.extend(["--target", spec.target])
    if spec.compile_only:
        cmd.append("--check-only")

    env = test_env()
    result = subprocess.run(
        cmd, capture_output=True, text=True, cwd=PROJECT_ROOT, env=env
    )

    if spec.compile_only and spec.expect_error:
        if result.returncode == 0:
            return TestResult(
                entry,
                "fail",
                f"\033[31;1mERROR:\033[0m Expected type check to fail for `{entry}`.",
            )
        return TestResult(
            entry,
            "pass",
            f"\033[32;1m PASS:\033[0m Test `{entry}` passed (expected error)!",
        )

    if result.returncode != 0:
        msg = f"\033[31;1mERROR:\033[0m Compilation stage failed for `{entry}`."
        if result.stderr:
            msg += f"\n{result.stderr}"
        return TestResult(entry, "fail", msg)

    if spec.compile_only:
        return TestResult(
            entry,
            "pass",
            f"\033[32;1m PASS:\033[0m Test `{entry}` passed (compile-only)!",
        )

    output_bin = entry.with_suffix("")
    if not output_bin.exists():
        msg = f"\033[31;1mERROR:\033[0m Output binary missing for `{entry}`."
        if result.stderr:
            msg += f"\n{result.stderr}"
        return TestResult(entry, "fail", msg)

    result = subprocess.run(
        output_bin, capture_output=True, text=True, cwd=PROJECT_ROOT
    )
    if result.returncode != spec.exit_code:
        return TestResult(
            entry,
            "fail",
            f"\033[31;1mERROR:\033[0m Execution stage failed for `{output_bin}`.\n"
            f"       expected exit code: {spec.exit_code}, actual: {result.returncode}",
        )

    return TestResult(
        entry,
        "pass",
        f"\033[32;1m PASS:\033[0m Test `{output_bin}` passed!",
    )


def discover_tests(directory: Path) -> list[Path]:
    return sorted(
        directory / entry
        for entry in os.listdir(directory)
        if entry.endswith(".rye")
    )


class LiveProgress:
    def __init__(self, total: int, jobs: int, enabled: bool = True) -> None:
        self.total = total
        self.jobs = jobs
        self.enabled = enabled and sys.stdout.isatty()
        self._lock = threading.Lock()
        self.done = 0
        self.passing = 0
        self.failing = 0
        self.skipping = 0
        self.latest = ""
        self.start = time.monotonic()
        self._spinner_i = 0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        if self.enabled:
            self._thread = threading.Thread(target=self._animate, daemon=True)
            self._thread.start()

    def _animate(self) -> None:
        while not self._stop.wait(0.07):
            with self._lock:
                self._spinner_i += 1
            self._draw()

    def _bar(self, frac: float, failing: int) -> str:
        filled = int(BAR_WIDTH * frac)
        empty = BAR_WIDTH - filled
        if failing:
            fill = f"\033[31m{'█' * filled}\033[0m"
        else:
            fill = f"\033[32m{'█' * filled}\033[0m"
        return fill + f"\033[90m{'░' * empty}\033[0m"

    def _draw(self) -> None:
        if not self.enabled:
            return
        with self._lock:
            elapsed = time.monotonic() - self.start
            frac = self.done / self.total if self.total else 1.0
            spin = SPINNER[self._spinner_i % len(SPINNER)]
            bar = self._bar(frac, self.failing)
            latest = self.latest
            done = self.done
            passing = self.passing
            failing = self.failing
            skipping = self.skipping

        line = (
            f"\r\033[2K  {spin} "
            f"\033[35;1mrye\033[0m  "
            f"{bar}  "
            f"{done}/{self.total}  "
            f"\033[32m✓{passing}\033[0m "
            f"\033[31m✗{failing}\033[0m "
            f"\033[33m⊘{skipping}\033[0m  "
            f"{elapsed:.1f}s"
        )
        if latest:
            line += f"  \033[90m· {latest}\033[0m"
        sys.stdout.write(line)
        sys.stdout.flush()

    def update(self, result: TestResult) -> None:
        with self._lock:
            self.done += 1
            if result.status == "pass":
                self.passing += 1
            elif result.status == "fail":
                self.failing += 1
            elif result.status == "skip":
                self.skipping += 1
            self.latest = result.path.name
        self._draw()

    def finish(self) -> float:
        if self._thread is not None:
            self._stop.set()
            self._thread.join(timeout=0.2)
        elapsed = time.monotonic() - self.start
        if self.enabled:
            sys.stdout.write("\r\033[2K")
            sys.stdout.flush()
        return elapsed


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the rye test suite.")
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=os.cpu_count() or 4,
        help="number of tests to run in parallel (default: CPU count)",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="disable live progress display",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="print every test result, not just failures",
    )
    parser.add_argument(
        "--batch",
        action="store_true",
        help="run tests in a single rye process (reuses std/runtime)",
    )
    args = parser.parse_args()

    specs: list[TestSpec] = []
    skipped = 0
    for entry in discover_tests(Path(DIRECTORY)):
        spec = parse_test(entry)
        if spec is None:
            print(f"\033[33;1m SKIP:\033[0m Invalid test `{entry}`, skipping.")
            skipped += 1
            continue
        specs.append(spec)

    if specs and not args.quiet:
        mode = "batch" if args.batch else f"{max(1, args.jobs)} workers"
        print(
            f"  \033[35;1mrye\033[0m · running {len(specs)} tests "
            f"({mode}) on {host_triple()}"
        )

    if not args.batch:
        warm_cache(specs)

    progress = LiveProgress(len(specs), args.jobs, enabled=not args.quiet)
    results: list[TestResult] = []
    if args.batch:
        results = run_batch(specs, progress)
    else:
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            futures = {pool.submit(run_test, spec): spec for spec in specs}
            for future in as_completed(futures):
                result = future.result()
                results.append(result)
                progress.update(result)

    elapsed = progress.finish()

    results.sort(key=lambda r: r.path.name)
    passing = failing = 0
    failures: list[TestResult] = []
    for result in results:
        if result.status == "pass":
            passing += 1
        elif result.status == "fail":
            failing += 1
            failures.append(result)
        elif result.status == "skip":
            skipped += 1

    if args.verbose:
        for result in results:
            print(result.message)
    elif failures:
        for result in failures:
            print(result.message)

    if failures or args.verbose:
        print("")
    if failing:
        tone = "\033[31;1m"
        verdict = "failed"
    else:
        tone = "\033[32;1m"
        verdict = "passed"
    print(
        f"  {tone}done\033[0m in {elapsed:.1f}s — "
        f"\033[32m{passing} passed\033[0m, "
        f"\033[33m{skipped} skipped\033[0m, "
        f"\033[31m{failing} failed\033[0m "
        f"({verdict})"
    )


if __name__ == "__main__":
    main()
