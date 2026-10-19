import sys
print(sys.version)
for m in ["simDocumentQuery","SimApi","simDocumentUtils","simDocUtils","simResultsReader","csimAccess","hsimAccess"]:
    try:
        mod = __import__(m)
        print("==", m, [x for x in dir(mod) if not x.startswith("__")][:80])
    except Exception as e:
        print("!!", m, repr(e))
