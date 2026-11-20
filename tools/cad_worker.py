"""Heavy tasks run out of the GUI process. On Windows, children share its lifetime."""
import json
import os
from pathlib import Path
import sys


def contain_children(memory_limit=None):
    if os.name!="nt":
        return
    import ctypes as ct
    from ctypes import wintypes as wt
    class Limits(ct.Structure):
        _fields_=[("processTime",ct.c_longlong),("jobTime",ct.c_longlong),("flags",wt.DWORD),
                  ("minWs",ct.c_size_t),("maxWs",ct.c_size_t),("active",wt.DWORD),
                  ("affinity",ct.c_size_t),("priority",wt.DWORD),("scheduling",wt.DWORD)]
    class Extended(ct.Structure):
        _fields_=[("basic",Limits),("io",ct.c_ulonglong*6),("memory",ct.c_size_t*4)]
    kernel=ct.WinDLL("kernel32",use_last_error=True)
    kernel.CreateJobObjectW.restype=wt.HANDLE
    job=kernel.CreateJobObjectW(None,None)
    data=Extended()
    data.basic.flags=0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if memory_limit is not None:
        data.basic.flags|=0x0200  # JOB_OBJECT_LIMIT_JOB_MEMORY
        data.memory[1]=memory_limit
    if not job or not kernel.SetInformationJobObject(wt.HANDLE(job),9,ct.byref(data),ct.sizeof(data)):
        raise ct.WinError(ct.get_last_error())
    if not kernel.AssignProcessToJobObject(wt.HANDLE(job),wt.HANDLE(-1)):
        raise ct.WinError(ct.get_last_error())
    # Retain this non-inheritable handle until process exit.


def run(request):
    task=request["task"]
    if task=="recovery":
        from cad_scene import save_scene
        save_scene(request["scene"],Path(request["assets"]),Path(request["path"]))
        return
    if task in ("checked_trace","trace"):
        import subprocess
        contain_children(request.get("memory_limit_bytes"))
        issues=[]
        if task=="checked_trace":
            from cad_inspect import inspect
            issues=inspect(request["scene"],Path(request["assets"]))
        if issues:
            Path(request["path"]).write_text(json.dumps(issues,ensure_ascii=False),encoding="utf-8")
        else:
            subprocess.run([request["cli"],*request["args"]],check=True)
    elif task=="inspect":
        from cad_inspect import inspect
        result=inspect(request["scene"],Path(request["assets"]))
        Path(request["path"]).write_text(json.dumps(result,ensure_ascii=False),encoding="utf-8")
    elif task=="placement":
        contain_children()
        from cad_placement import search
        result=search(request)
        Path(request["path"]).write_text(json.dumps(result,ensure_ascii=False),encoding="utf-8")
    elif task=="export":
        if os.environ.get("RAYSCULPT_CAD_EXPORT"):
            contain_children()
        from cad_geometry import export_body
        export_body(request["object"],Path(request["assets"]),Path(request["path"]),request.get("cells"))
    elif task=="import":
        import numpy as np
        from cad_geometry import visual_geometry
        xyz,tri,volume=visual_geometry(request["object"],Path(request["assets"]))
        np.savez(request["path"],xyz=xyz,tri=tri,volume=np.nan if volume is None else volume)
    elif task=="fit":
        contain_children()
        from cad_fit import fit
        fit(request["scene"],Path(request["assets"]),Path(request["folder"]))
    elif task=="design":
        import subprocess
        contain_children()
        subprocess.run([sys.executable,*request["args"]],check=True)
    else:
        raise ValueError("Unknown CAD operation")


if __name__=="__main__":
    try:
        run(json.loads(Path(sys.argv[1]).read_text(encoding="utf-8")))
    except Exception as exc:
        if isinstance(exc,MemoryError) or getattr(exc,"winerror",0) in (8,14,1455,1816):
            print(json.dumps(dict(event="error",detail=dict(code="memory",message=str(exc)))),file=sys.stderr)
        else:
            print(str(exc),file=sys.stderr)
        sys.exit(1)
