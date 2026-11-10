"""Compare two GZPro asset registries entry by entry (resolved names, tag values with their types, bundles, chunks, flags).
   compare_registries.py <Kiryu tools folder> <a.bin> <b.bin>   (prints "same entries in the same order: True/False")"""
import sys
sys.path.insert(0, sys.argv[1])
import assetregistry as ar

def canon(path):
    r = ar.Registry(open(path, "rb").read())
    out = []
    for a in r.assets:
        tags = tuple((k, v & 7, r.value_str(v)) for k, v in r.tag_pairs(a))
        bundles = tuple((r.name(bn), tuple((r.name(p), r.name(o), s) for p, o, s in items)) for bn, items in a.bundles)
        out.append((r.object_path(a), r.name(a.package_path), r.name(a.class_package), r.name(a.class_name),
                    tags, bundles, tuple(a.chunks), a.flags))
    return out

A, B = canon(sys.argv[2]), canon(sys.argv[3])
print(len(A), "entries vs", len(B))
same_order = A == B
print("same entries in the same order:", same_order)
if not same_order:
    print("same set:", sorted(A) == sorted(B))
    sa, sb = set(A), set(B)
    for x in list(sa - sb)[:5]: print("  only in a:", x[0])
    for x in list(sb - sa)[:5]: print("  only in b:", x[0])
