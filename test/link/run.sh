#!/bin/bash
# Regression test for pinned-section retention.
#
# A fixed-origin BSS[0] section whose symbols are referenced only through
# cross-section constant uses (which the assembler folds to immediates,
# producing no relocation) must still be kept by the linker, so the following
# free BSS section is placed after it. If the pinned section is dropped, the
# free section overlaps it and "sda" lands at $0000 instead of $0002.

BIN=../../build/cmake/debug
rm -f pinnedbss.obj pinnedbss.map pinnedbss.bin

"$BIN/xasm/rc8/motorrc8" -g -el -opinnedbss.obj pinnedbss.rc8 || { echo "FAIL: assemble"; exit 1; }
"$BIN/xlink/xlink" -chc800b -fhc800k -sStart -mpinnedbss.map -opinnedbss.bin pinnedbss.obj || { echo "FAIL: link"; exit 1; }

if ! grep -q '^\$0000 | freeSectors' pinnedbss.map || ! grep -q '^\$0002 | sda' pinnedbss.map; then
    echo "FAIL: pinned BSS section not retained (expected freeSectors@\$0000, sda@\$0002)"
    exit 1
fi
rm -f pinnedbss.obj pinnedbss.map pinnedbss.bin

# Regression test for labels exported from a fixed-origin section.
#
# The assembler keeps such labels at their absolute address. If it writes that
# value for an EXPORT, xlink adds the section's location again, so another
# object importing "Start" sees $81000000 instead of $40800000.
#
# fixedexport.def also writes its pool banks as a bare "-1", which must not
# be read as a subtraction from the address before it.

rm -f fixedexport.obj fixedexport2.obj fixedexport.bin

"$BIN/xasm/680x0/motor68k" -mc0 -fx -ofixedexport.obj fixedexport.68k || { echo "FAIL: assemble"; exit 1; }
"$BIN/xasm/680x0/motor68k" -mc0 -fx -ofixedexport2.obj fixedexport2.68k || { echo "FAIL: assemble"; exit 1; }
"$BIN/xlink/xlink" -afixedexport.def -fbin -ofixedexport.bin fixedexport.obj fixedexport2.obj || { echo "FAIL: link"; exit 1; }

if [ "$(od -An -tx1 fixedexport.bin | tr -d ' \n')" = "1234567840800000408000006000fff2" ]; then
    rm -f fixedexport.obj fixedexport2.obj fixedexport.bin
    exit 0
fi

echo "FAIL: label exported from fixed section relocated twice"
exit 1
