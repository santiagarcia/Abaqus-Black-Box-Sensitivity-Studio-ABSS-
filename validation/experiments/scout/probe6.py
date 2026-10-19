import csimAccess as c
for cls in ["SMACSimDistribution","SMACSimGrid","SMACSimDrawerDescription","SMACSimCSet","SMACSimCollection","SMACSimDataRecord"]:
    k=getattr(c,cls)
    print("==",cls)
    for n in dir(k):
        if n.startswith("_"): continue
        doc=(getattr(k,n).__doc__ or "").strip().replace("\n"," ")
        import re; doc=re.sub(r"\s+"," ",doc)[:260]
        print("  ",n,"|",doc)
