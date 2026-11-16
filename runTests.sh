#!/bin/bash
# Runs the metric tests (unit tests and oracle scenarios) via ctest. Build first with ./buildTests.sh.
set -e
cd "$(dirname "$0")/build"

ctest --output-on-failure
