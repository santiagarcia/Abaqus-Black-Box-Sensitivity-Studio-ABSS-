# -*- coding: utf-8 -*-
"""Dump nodal and integration-point field output of an Abaqus ODB to JSON.

Run with the Abaqus Python interpreter (Abaqus 2024 ships Python 3.10):

    abaqus python odb_dump.py JOB.odb OUT.json [--fields U,RF,CF,S,PEEQ]
                                                [--mesh] [--last-frame-only]

Output schema (all keys are strings because JSON objects need string keys):

    {
      "odb": path, "abaqus_release": str,
      "mesh": {instance: {"nodes": {label: [x, y, (z)]},
                          "elements": {label: {"type": str, "conn": [...]}}}},
      "steps": [
        {"name": str, "procedure": str, "total_time_at_start": float,
         "frames": [
           {"frame_id": int, "increment": int, "step_time": float,
            "total_time": float, "description": str,
            "fields": {
              NAME: {"position": "NODAL" | "INTEGRATION_POINT" | ...,
                     "precision": "SINGLE_PRECISION" | "DOUBLE_PRECISION",
                     "components": [...],
                     "values": [[instance, nodeOrElemLabel, ip, sectionPoint,
                                 [v1, v2, ...]], ...]}}}]}]}

Values are read from ``dataDouble`` when the ODB stores double precision
(e.g. when the job ran with ``output_precision=full``), else from ``data``.
Nothing is averaged or projected: integration-point values keep their
element label and integration point number.

This script is intentionally dependency-free (only the Abaqus odbAccess
module and the standard library) so that the ABSS C++ application can call
it as a subprocess.
"""
from __future__ import print_function

import json
import sys

from odbAccess import openOdb  # noqa: E402  (Abaqus-only module)


def _as_list(value):
    try:
        return [float(x) for x in value]
    except TypeError:
        return [float(value)]


def _field_values(field):
    out = []
    precision = None
    for v in field.values:
        if precision is None:
            precision = str(v.precision)
        data = v.dataDouble if str(v.precision) == "DOUBLE_PRECISION" else v.data
        inst = v.instance.name if v.instance is not None else ""
        if v.nodeLabel is not None and v.elementLabel is None:
            label = int(v.nodeLabel)
        elif v.elementLabel is not None:
            label = int(v.elementLabel)
        else:
            label = int(v.nodeLabel) if v.nodeLabel is not None else -1
        ip = int(v.integrationPoint) if v.integrationPoint is not None else 0
        sp = int(v.sectionPoint.number) if v.sectionPoint is not None else 0
        node = int(v.nodeLabel) if (v.nodeLabel is not None and v.elementLabel is not None) else 0
        out.append([inst, label, ip, sp, _as_list(data), node])
    return out, precision


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    odb_path, out_path = argv[1], argv[2]
    wanted = None
    want_mesh = False
    last_only = False
    i = 3
    while i < len(argv):
        if argv[i] == "--fields":
            wanted = [s.strip() for s in argv[i + 1].split(",") if s.strip()]
            i += 2
        elif argv[i] == "--mesh":
            want_mesh = True
            i += 1
        elif argv[i] == "--last-frame-only":
            last_only = True
            i += 1
        else:
            raise SystemExit("unknown argument " + argv[i])

    odb = openOdb(odb_path, readOnly=True)
    result = {"odb": odb_path, "abaqus_release": str(odb.jobData.version), "steps": []}
    if want_mesh:
        mesh = {}
        for name, inst in odb.rootAssembly.instances.items():
            nodes = dict((str(n.label), list(n.coordinates)) for n in inst.nodes)
            elems = dict(
                (str(e.label), {"type": e.type, "conn": list(e.connectivity)})
                for e in inst.elements
            )
            mesh[name] = {"nodes": nodes, "elements": elems}
        result["mesh"] = mesh

    for step_name in odb.steps.keys():
        step = odb.steps[step_name]
        sd = {
            "name": step_name,
            "procedure": step.procedure,
            "total_time_at_start": float(step.totalTime),
            "frames": [],
        }
        frames = list(step.frames)
        if last_only:
            frames = frames[-1:]
        for fr in frames:
            fd = {
                "frame_id": int(fr.frameId),
                "increment": int(fr.incrementNumber),
                "step_time": float(fr.frameValue),
                "total_time": float(step.totalTime + fr.frameValue),
                "description": fr.description,
                "fields": {},
            }
            for fname in fr.fieldOutputs.keys():
                if wanted is not None and fname not in wanted:
                    continue
                fo = fr.fieldOutputs[fname]
                vals, prec = _field_values(fo)
                pos = str(fo.locations[0].position) if len(fo.locations) else ""
                fd["fields"][fname] = {
                    "position": pos,
                    "precision": prec,
                    "components": list(fo.componentLabels),
                    "values": vals,
                }
            sd["frames"].append(fd)
        result["steps"].append(sd)
    odb.close()
    with open(out_path, "w") as fh:
        json.dump(result, fh)
    print("wrote", out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
