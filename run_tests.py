from pathlib import Path
import subprocess
import os

COMPILER = "./build/rye"
DIRECTORY = "./tests/"

def parse_test(path: Path) -> (bool, int, list[str]):
    with open(path, 'r') as f:
        lines = f.readlines();
    if not lines[0].startswith('/// '):
        return (False, 0, [])

    exit_code = int(lines[0][3:].strip())
    import_paths = []
    for line in lines[1:]:
        line = line.strip()
        if not line.startswith('/// '):
            break
        if line.startswith('/// import-path:'):
            import_paths.append(line[len('/// import-path:'):].strip())
    return (True, exit_code, import_paths)

passing, skipped, failing = 0,0,0
for entry in os.listdir(DIRECTORY):
    entry = Path(os.path.join(DIRECTORY, entry))
    if entry.suffix == ".rye":
        (success, test_exit_code, import_paths) = parse_test(entry)
        if not success:
            print(f"\033[33;1m SKIP:\033[0m Invalid test `{entry}`, skipping.")
            skipped += 1
            continue

        cmd = [COMPILER, entry, "-O", entry.with_suffix("")]
        for import_path in import_paths:
            cmd.extend(["-I", import_path])
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"\033[31;1mERROR:\033[0m Compilation stage failed for `{entry}`.")
            if result.stderr:
                print(result.stderr)
            failing += 1
            continue

        output_bin = entry.with_suffix("")
        if not output_bin.exists():
            print(f"\033[31;1mERROR:\033[0m Output binary missing for `{entry}`.")
            if result.stderr:
                print(result.stderr)
            failing += 1
            continue

        result = subprocess.run(output_bin, capture_output=True, text=True)
        if result.returncode != test_exit_code:
            print(f"\033[31;1mERROR:\033[0m Execution stage failed for `{output_bin}`.")
            print(f"       expected exit code: {test_exit_code}, actual: {result.returncode}")
            failing += 1
            continue

        print(f"\033[32;1m PASS:\033[0m Test `{output_bin}` passed!")
        passing += 1

print(f"")
print(f"All tests completed.")
print(f"  \033[32;1m{passing} \033[33;1m{skipped} \033[31;1m{failing} \033[0m")
