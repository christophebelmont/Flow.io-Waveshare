#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ -f "scripts/generate_config_docs.py" ]]; then
  python3 scripts/generate_config_docs.py
fi

if [[ -f "scripts/generate_cfgdoc_chunks.py" ]]; then
  python3 scripts/generate_cfgdoc_chunks.py
fi

assets=(
  "data/webinterface/index.html"
  "data/webinterface/sh.html"
  "data/webinterface/app.js"
  "data/webinterface/i18n/fr.json"
  "data/webinterface/i18n/en.json"
  "data/webinterface/app-core.css"
  "data/webinterface/app-core.js"
  "data/webinterface/light.html"
  "data/webinterface/light.css"
  "data/webinterface/light.js"
  "data/webinterface/prov.html"
  "data/webinterface/prov.js"
  "data/webinterface/runtimeui.json"
)

for asset in "${assets[@]}"; do
  if [[ -f "$asset" ]]; then
    gzip -n -9 -c "$asset" > "$asset.gz"
  fi
done

if [[ -d "data/wc" ]]; then
  while IFS= read -r -d '' file; do
    gzip -n -9 -c "$file" > "${file}.gz"
  done < <(find "data/wc" -type f -name '*.j' -print0)
fi

rm -f \
  "data/webinterface/cfgdocs.json" \
  "data/webinterface/cfgmods.json" \
  "data/webinterface/cfgdocs.jz" \
  "data/webinterface/cfgmods.jz"
