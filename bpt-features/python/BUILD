load("@rules_python//python:py_test.bzl", "py_test")

# Smoke test for the Python bindings. Lives at python/ level (not
# inside bpt_features/) so the test imports `bpt_features` as a
# package rather than from inside the package.
py_test(
    name = "test_ofi_smoke",
    srcs = ["tests/test_ofi_smoke.py"],
    deps = ["//bpt-features/python/bpt_features"],
)
