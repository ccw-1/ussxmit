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

# 6. Pipeline streaming PDS: xmit -> receive
echo "Test 6: Streaming PDS pipeline: xmit -> receive"
$BIN xmit "CCW.TESTPDS" | $BIN receive -f "CCW.TESTPDS.INTTEST"

# 7. Verify restored PDS dataset via re-xmit pipeline
echo "Test 7: Verify restored PDS dataset via re-xmit pipeline"
$BIN xmit "CCW.TESTPDS.INTTEST" | $BIN inspect | grep "Dataset Name : CCW.TESTPDS.INTTEST"
$BIN xmit "CCW.TESTPDS.INTTEST" | $BIN inspect | grep "Organization : PO (Partitioned)"

# 8. PDSE (data library, via IEBCOPY): xmit -> receive -> same members
echo "Test 8: Streaming PDSE pipeline: xmit -> receive"
$BIN xmit "CCW.TESTPDSE" | $BIN receive -f "CCW.TESTPDSE.INTTEST"
for m in MEM1 MEM2 MEM3; do
    a=$(cat "//'CCW.TESTPDSE($m)'" | cksum)
    b=$(cat "//'CCW.TESTPDSE.INTTEST($m)'" | cksum)
    [ "$a" = "$b" ] || { echo "PDSE member $m differs"; exit 1; }
done

echo ""
echo "=================================================="
echo " All Integration Tests Passed Successfully!"
echo "=================================================="
