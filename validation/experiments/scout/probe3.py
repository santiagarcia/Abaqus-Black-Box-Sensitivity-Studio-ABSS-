import csimAccess as c, inspect
for cls in ["SMACSimDocument","SMACSimDrawer","SMACSimNavObject","SMACSimObject","SMACSimDSet"]:
    k=getattr(c,cls)
    print("==",cls)
    for n in dir(k):
        if n.startswith("_"): continue
        doc=(getattr(k,n).__doc__ or "").strip().replace("\n"," ")[:160]
        print("  ",n,"|",doc)
