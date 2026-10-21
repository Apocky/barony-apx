import re
lines = open(r"C:\Users\Apocky\source\repos\Apockalypsis\specs\barony-fork\04b-COMBAT-INPUT-REWRITE.md").read().split('\n')
print(f"total: {len(lines)}")
# print the C3 section and any perf/p99 mentions with line numbers
for i, l in enumerate(lines):
    if re.search(r'(?i)(C3|C1|p99|perf|crowd|gate)', l):
        print(f"{i+1}: {l[:240]}")
