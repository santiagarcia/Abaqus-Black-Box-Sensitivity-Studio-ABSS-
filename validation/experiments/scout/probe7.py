import csimAccess as c, re
print([x for x in dir(c) if "Sparse" in x or "Type" in x or "Const" in x])
d=c.SMACSimDistribution
for n in ["ReadSparse","Read","Drawers","RangeType"]:
    print(n, re.sub(r"\s+"," ",getattr(d,n).__doc__ or ""))
import csimConstants as k
print([x for x in dir(k) if not x.startswith("_")][:200])
