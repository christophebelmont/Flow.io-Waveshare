"""Opt-in diagnostics without replacing the project's normal build flags."""
import os

Import("env")

value = os.environ.get("FLOW_MEMORY_DIAGNOSTICS", "0")
if value not in ("0", "1"):
    raise ValueError("FLOW_MEMORY_DIAGNOSTICS must be 0 or 1")
env["FLOW_MEMORY_DIAGNOSTICS"] = value == "1"
if value == "1":
    env.Append(CPPDEFINES=[("FLOW_MEMORY_DIAGNOSTICS", 1)])
    env.Append(CCFLAGS=["-fstack-usage"])
    print("[memory] diagnostics ON; firmware includes WM traces and compiler .su files")
