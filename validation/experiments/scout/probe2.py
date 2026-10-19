import inspect, simDocumentQuery as q, csimAccess as c
for n in ["test","analysisType","getAncestry"]:
    f=getattr(q,n)
    try: print(n, inspect.signature(f), (f.__doc__ or "")[:300])
    except Exception as e: print(n, e)
print(q.analTypeMap)
d=c.SMACSimDocument
print([x for x in dir(d) if not x.startswith("_")])
print(c.SMACSimDocument.__init__.__doc__)
