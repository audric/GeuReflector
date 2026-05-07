#!/usr/bin/env bash
# Build docs/TOPOLOGY_EXAMPLES.pdf from the Graphviz sources and HTML
# template under docs/topology_pdf/.
#
# Dependencies:
#   - graphviz (dot, circo, neato)
#   - weasyprint
#
# The Markdown source (docs/TOPOLOGY_EXAMPLES.md) is independent — this
# script does not consume it. Update both if you change the prose.

set -euo pipefail

cd "$(dirname "$0")/topology_pdf"

echo "[1/3] Rendering SVG diagrams"
circo -Tsvg 01_intl_mesh.dot      -o 01_intl_mesh.svg
neato -Tsvg 02_flat_mesh.dot      -o 02_flat_mesh.svg
dot   -Tsvg 03_satellite_tree.dot -o 03_satellite_tree.svg
neato -Tsvg 04_hybrid.dot         -o 04_hybrid.svg
neato -Tsvg 05_twin_pair.dot      -o 05_twin_pair.svg

echo "[2/3] Building PDF with WeasyPrint"
BUILD_DATE=$(date -u +"%Y-%m-%d %H:%M UTC")
sed "s|__BUILD_DATE__|$BUILD_DATE|g" topology.html > topology.rendered.html
weasyprint topology.rendered.html ../TOPOLOGY_EXAMPLES.pdf
rm topology.rendered.html

echo "[3/3] Done — wrote docs/TOPOLOGY_EXAMPLES.pdf ($(stat -c%s ../TOPOLOGY_EXAMPLES.pdf) bytes)"
