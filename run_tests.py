from pathlib import Path
import subprocess
import os

COMPILER = "./build/rye"
DIRECTORY = "./tests/"

def parse_test(path: Path) -> (bool, int):
    with open(path, 'r') as f:
        lines = f.readlines();
    if not lines[0].startswith('/// '):
        return (False, 0)

    exit_code = int(lines[0][3:].strip())
    return (True, exit_code)

passing, skipped, failing = 0,0,0
for entry in os.listdir(DIRECTORY):
    entry = Path(os.path.join(DIRECTORY, entry))
    if entry.suffix == ".rye":
        cmd = [COMPILER, entry, "-O", entry.with_suffix("")]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"\033[31;1mERROR:\033[0m Compilation stage failed for `{entry}`.")
            failing += 1
            continue

        (success, test_exit_code) = parse_test(entry)
        if not success:
            print(f"\033[33;1m SKIP:\033[0m Invalid test `{entry}`, skipping.")
            skipped += 1
            continue

        entry = entry.with_suffix("")
        result = subprocess.run(entry, capture_output=True, text=True)
        if result.returncode != test_exit_code:
            print(f"\033[31;1mERROR:\033[0m Execution stage failed for `{entry}`.")
            print(f"       expected exit code: {test_exit_code}, actual: {result.returncode}")
            failing += 1
            continue

        print(f"\033[32;1m PASS:\033[0m Test `{entry}` passed!")
        passing += 1

print(f"")
print(f"All tests completed.")
print(f"  \033[32;1m{passing} \033[33;1m{skipped} \033[31;1m{failing} \033[0m")
