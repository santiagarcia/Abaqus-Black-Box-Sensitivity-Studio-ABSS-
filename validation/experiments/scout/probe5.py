import csimAccess as c, sys
KINDS=["DataRecords","DSets","Collections","Distributions","FTables"]
def walk(obj, depth, maxd=7):
    try: rels=obj.ChildRelations()
    except Exception as e: return
    for r in rels:
        role=r[0]
        try: n=obj.NumChildren(role)
        except Exception as e: n="?"
        found=False
        for k in KINDS:
            try:
                kids=getattr(obj,"Children"+k)(role)
            except Exception as e:
                continue
            try: kids=list(kids)
            except Exception: kids=[kids]
            if not kids: continue
            found=True
            print("  "*depth+f"{role} [{k}] n={len(kids)}")
            for ch in kids[:3]:
                try: nm=ch.Name()
                except Exception: nm="?"
                try: tn=ch.TypeName()
                except Exception: tn="?"
                print("  "*depth+f"  - {nm} : {tn}")
                if depth<maxd and k in ("DataRecords","Collections"): walk(ch, depth+2, maxd)
            break
        if not found: print("  "*depth+f"{role} n={n} (no kids read)")
d=c.SMACSimDocument(); d.Load(sys.argv[1], True); walk(d.Focus(),0); d.Close()
