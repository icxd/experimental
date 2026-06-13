#!/bin/sh
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$ROOT/editor/vscode-rye"
OUT="$EXT/rye-vscode.vsix"

if ! command -v npm >/dev/null 2>&1; then
  echo "npm not found. Install Node.js to package the extension."
  exit 1
fi

cd "$EXT"
npm install --omit=dev

if ! command -v vsce >/dev/null 2>&1; then
  echo "vsce not found. Install with: npm install -g @vscode/vsce"
  echo ""
  echo "Or load the extension for development without packaging:"
  echo "  VS Code / Cursor: Extensions → ... → Install from VSIX / Load unpacked"
  echo "  Path: $EXT"
  exit 1
fi

cd "$EXT"
vsce package --out "$OUT" --allow-missing-repository
echo "Packaged: $OUT"
echo ""
echo "Install in Cursor (pick one):"
echo "  UI: Extensions sidebar → ... → Install from VSIX... → select:"
echo "      $OUT"
echo ""
CURSOR_CLI="/Applications/Cursor.app/Contents/Resources/app/bin/cursor"
if [ -x "$CURSOR_CLI" ]; then
  echo "  CLI: $CURSOR_CLI --install-extension $OUT"
else
  echo "  CLI: cursor --install-extension $OUT"
  echo "       (If 'cursor' is not found, run in Cursor: Shell Command: Install 'cursor' command in PATH)"
fi
echo ""
echo "Install in VS Code:"
if command -v code >/dev/null 2>&1; then
  echo "  code --install-extension $OUT"
else
  echo "  Extensions → ... → Install from VSIX... → $OUT"
fi
