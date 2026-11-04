#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$ROOT_DIR/bin/ussxmit"

echo "=================================================="
echo " Running ussxmit End-to-End Integration Tests"
echo "=================================================="

# 1. Version and Help
echo "Test 1: Version and Help options"
$BIN -V
$BIN -h >/dev/null

# 2. Inspect MVS dataset directly
echo "Test 2: Inspect MVS dataset directly"
$BIN inspect "CCW.TESTPDS.XMI" | grep "Dataset Name : CCW.TESTPDS"
$BIN inspect "CCW.TESTPDS.XMI" | grep "Utility Name : IEBCOPY"

# 3. Pipeline streaming: xmit -> inspect
echo "Test 3: Streaming pipeline: xmit -> inspect"
$BIN xmit "CCW.TESTSEQ" | $BIN inspect | grep "Dataset Name : CCW.TESTSEQ"

# 4. Pipeline streaming: xmit -> receive
echo "Test 4: Streaming pipeline: xmit -> receive"
$BIN xmit "CCW.TESTSEQ" | $BIN receive -f "CCW.TESTSEQ.INTTEST"

# 5. Verify restored dataset via re-xmit pipeline
echo "Test 5: Verify restored dataset via re-xmit pipeline"
$BIN xmit "CCW.TESTSEQ.INTTEST" | $BIN inspect | grep "Dataset Name : CCW.TESTSEQ.INTTEST"

echo ""
echo "=================================================="
echo " All Integration Tests Passed Successfully!"
echo "=================================================="
