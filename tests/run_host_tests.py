from pathlib import Path
import subprocess,tempfile
p=Path(__file__).resolve().parents[1]
code = r'''#include "bsp_nfc_policy.hpp"
#include <cassert>
int main(){BspNfcPlacement p;assert(p.observe("AABBCCDD"));assert(!p.observe("AABBCCDD"));p.observe("");p.observe("");assert(!p.observe("AABBCCDD"));p.observe("");p.observe("");p.observe("");assert(p.observe("AABBCCDD"));assert(p.observe("11223344"));p.reset();assert(p.observe("11223344"));}
'''
with tempfile.TemporaryDirectory() as d:
 src=Path(d)/"test.cpp";src.write_text(code)
 subprocess.run(["c++","-std=c++17","-I",str(p/"components/bsp/include"),str(src),"-o",d+"/test"],check=True)
 subprocess.run([d+"/test"],check=True)
print("PASS NFC placement dedup/rearm/change/reset")
