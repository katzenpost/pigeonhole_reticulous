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
    if isinstance(doc, dict) and "queries" in doc:  # pigeonhole_rns bridge vectors: one list per kind
        vectors = [(k + "/" + v["name"], v) for k in ("queries", "replies", "malformed", "boxes") for v in doc[k]]
    elif isinstance(doc, dict) and "vectors" in doc:
        vectors = [(str(v.get("name", i)), v) for i, v in enumerate(doc["vectors"])]
    elif isinstance(doc, dict):  # one vector per section, named by it
        vectors = [(k, v) for k, v in doc.items() if isinstance(v, dict)]
    else:
        vectors = [(str(i), v) for i, v in enumerate(doc)]
    for name, v in vectors:
        fields = [f"{k}={flat(x)}" for k, x in v.items()]
        assert not any("\t" in f or "\n" in f for f in fields), (path, name)
        out.append("\t".join([tag, name] + fields))
print("\n".join(out))
