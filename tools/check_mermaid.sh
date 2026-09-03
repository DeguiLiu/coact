#!/usr/bin/env bash
# Validate every mermaid block in a markdown file with the real mermaid parser.
# Usage: tools/check_mermaid.sh <file.md>...
# Requires: node + local mermaid/jsdom install (auto-bootstraps to /tmp/mmtest).
set -euo pipefail
[[ $# -ge 1 ]] || { echo "usage: $0 <file.md>..." >&2; exit 2; }

if [[ ! -d /tmp/mmtest/node_modules/mermaid ]]; then
    npm install --prefix /tmp/mmtest --silent mermaid@10 jsdom >/dev/null 2>&1
fi

cat > /tmp/mmtest/parse_one.mjs << 'EOF'
import { JSDOM } from '/tmp/mmtest/node_modules/jsdom/lib/api.js';
const dom = new JSDOM('<!DOCTYPE html><body></body>');
global.window = dom.window; global.document = dom.window.document;
const mermaid = (await import('/tmp/mmtest/node_modules/mermaid/dist/mermaid.esm.min.mjs')).default;
mermaid.initialize({ startOnLoad: false });
import { readFileSync } from 'fs';
try { await mermaid.parse(readFileSync(process.argv[2], 'utf8')); console.log('OK'); }
catch (e) { console.log('FAIL:', String(e.message || e).split('\n').slice(0,4).join(' | ')); process.exitCode = 1; }
EOF

fail=0
for md in "$@"; do
    awk -v out="/tmp/mmchk" '
        BEGIN { f = 0; n = 0; buf = "" }
        /^```mermaid$/ { f=1; n++; buf=""; next }
        /^```$/ { if (f) { f=0; printf "%s", buf > (out "_" n ".mmd") } next }
        f { buf = buf $0 "\n" }
    ' "$md"
    for blk in /tmp/mmchk_*.mmd; do
        [[ -e "$blk" ]] || continue
        res=$(node /tmp/mmtest/parse_one.mjs "$blk") || true
        echo "$(basename "$md") block $(basename "$blk" | sed 's/mmchk_//;s/.mmd//'): $res"
        [[ "$res" == OK ]] || fail=1
        rm -f "$blk"
    done
done
exit $fail
