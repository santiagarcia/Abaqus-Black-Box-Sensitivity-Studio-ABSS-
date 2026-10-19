import csimAccess as c, sys
for fn in sys.argv[1:]:
    d=c.SMACSimDocument()
    try:
        d.Load(fn, True)
    except Exception as e:
        print("load fail", fn, e); continue
    f=d.Focus()
    print("==",fn, f.TypeName())
    try:
        v=c.SMACSimRelationTypeVector() if hasattr(c,"SMACSimRelationTypeVector") else None
    except Exception as e: v=None
    try:
        rels=f.ChildRelations()
        print(rels)
    except Exception as e:
        print("ChildRelations err", e)
    d.Close()
