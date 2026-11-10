#!/usr/bin/env python3
"""Turn results-*.json into a Markdown table."""
import glob
import json
import os
import sys

art = sys.argv[1] if len(sys.argv) > 1 else "/artifacts"
print("# Test results\n")
total = failed = skipped = 0
for path in sorted(glob.glob(os.path.join(art, "results-*.json"))):
    results = json.load(open(path))
    print("## %s\n" % results[0]["suite"] if results else path)
    print("| Result | Test | Detail | Artifacts |")
    print("|---|---|---|---|")
    for r in results:
        total += 1
        failed += r["status"] not in ("PASS", "SKIP")
        skipped += r["status"] == "SKIP"
        detail = r["detail"].replace("|", "\\|").replace("\n", " ")[:400]
        arts = ", ".join("test-artifacts/" + os.path.basename(a)
                         for a in r["artifacts"])
        print("| %s | %s | %s | %s |" % (r["status"], r["test"], detail,
                                         arts))
    print()
print("**%d tests, %d passed, %d failed%s**" % (
    total, total - failed - skipped, failed,
    ", %d skipped" % skipped if skipped else ""))
