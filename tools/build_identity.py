"""Embed source identity; never stamp a dirty build as clean."""
Import("env")
import subprocess
root=env.subst("$PROJECT_DIR")
sha=subprocess.check_output(["git","rev-parse","HEAD"],cwd=root,text=True).strip()
if subprocess.check_output(["git","status","--porcelain"],cwd=root,text=True).strip():
    sha+="-dirty"
env.Append(CPPDEFINES=[("M5CAN_BUILD_SHA", '\\"'+sha+'\\"')])
