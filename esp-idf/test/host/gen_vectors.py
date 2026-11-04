"""Flattens JSON vector files into vectors.txt for the host tests: one vector per line,
`file<TAB>name<TAB>key=value...`, nested values as compact JSON."""
import json
import os
import sys


def flat(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (dict, list)):
        return json.dumps(v, separators=(",", ":"))
    return str(v)


out = []
for path in sys.argv[1:]:
    tag = os.path.relpath(path, os.path.dirname(os.path.abspath(__file__))).replace("vectors/", "")
    doc = json.load(open(path))
    vectors = doc["vectors"] if isinstance(doc, dict) and "vectors" in doc else doc
    for i, v in enumerate(vectors):
        fields = [f"{k}={flat(x)}" for k, x in v.items()]
        assert not any("\t" in f or "\n" in f for f in fields), (path, i)
        out.append("\t".join([tag, str(v.get("name", i))] + fields))
print("\n".join(out))
