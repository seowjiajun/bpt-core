load("@pybind11_bazel//:build_defs.bzl", "pybind_extension")
load("@rules_python//python:py_library.bzl", "py_library")

# Compiled C++ extension. Output lands as bpt_features/_core.so
# (relative to this BUILD), which is exactly where __init__.py expects
# to find it via `from bpt_features._core import ...`.
pybind_extension(
    name = "_core",
    srcs = ["bindings.cpp"],
    deps = [
        "//bpt-features:features",
    ],
    visibility = ["//visibility:public"],
)

# Pure-Python wrapper package. `data = [":_core"]` bundles the
# compiled .so so it ships alongside the .py files in any
# py_library that depends on this target.
py_library(
    name = "bpt_features",
    srcs = glob(["*.py"]),
    data = [":_core"],
    imports = [".."],  # so `import bpt_features` resolves at the python/ root
    visibility = ["//visibility:public"],
)
