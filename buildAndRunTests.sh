#!/bin/bash
set -e
cd "$(dirname "$0")"

./buildTests.sh
echo "=========== Running Tests ==========="
./runTests.sh
