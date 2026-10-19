import csimAccess as c, sys
d=c.SMACSimDocument(); d.Load(sys.argv[1], True)
m=d.Focus().ChildrenDataRecords("model")[0]
systems=m.ChildrenDataRecords("systems")
print("nsys",len(systems))
for s in systems:
    ums=s.ChildrenDistributions("userMatrices")
    for um in ums:
        try: print(" range", um.RangeType(), "masked", um.Masked())
        except Exception as e: print(" err", e)
        try:
            dr=um.Drawers()
            for x in dr:
                print("  drawer", x.TypeName(), x.DimensionSizes(), x.Description())
        except Exception as e: print("  drawers err", e)
d.Close()
