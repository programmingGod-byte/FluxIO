from setuptools import setup, Extension
import sys
import os

try:
    from pybind11.setup_helpers import Pybind11Extension, build_ext
except ImportError:
    from setuptools.command.build_ext import build_ext
    class Pybind11Extension(Extension):
        pass

__version__ = "1.0.0"

extra_compile_args = ["-std=c++20", "-O3", "-Wno-interference-size"]
extra_link_args = ["-lpthread"]

# Check for liburing
libraries = ["pthread"]
if os.path.exists("/usr/include/liburing.h") or os.path.exists("/usr/local/include/liburing.h"):
    libraries.append("uring")

include_dirs = [
    "include",
    "extern/pybind11/include",
]

ext_modules = [
    Pybind11Extension(
        "fluxio",
        ["python/bindings.cpp"],
        include_dirs=include_dirs,
        libraries=libraries,
        extra_compile_args=extra_compile_args,
        extra_link_args=extra_link_args,
        language="c++",
    ),
]

setup(
    name="fluxio",
    version=__version__,
    author="Shivam",
    description="Ultra-low latency asynchronous storage engine in modern C++20 for Python",
    long_description="",
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
    zip_safe=False,
    python_requires=">=3.8",
)
