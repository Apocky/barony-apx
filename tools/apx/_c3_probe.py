import re
lines = open(r"C:\Users\Apocky\source\repos\barony-apx-combat\tools\apx\combat_c3_gate.py").read().split('\n')
print(f"total: {len(lines)}")
for i, l in enumerate(lines):
    if re.search(r'(?i)(step|def |perf|crowd|dummy|fight|hostility|calm|seed|p99|0\.3|combat_qa|--)', l):
        print(f"{i+1}: {l[:240]}")
