from setuptools import Extension, setup
from Cython.Build import cythonize

sourcefiles = [
    "../logger/logger.c",
    "../dev_handler/devices.c",
    "../shm_wrapper/shm_wrapper.c",
    "../shm_wrapper_aux/shm_wrapper_aux.c",
    "../runtime_util/runtime_util.c",
    "studentapi.pyx"
]

setup(
    name="Student API",
    ext_modules = cythonize([
        Extension("studentapi", sources=sourcefiles, libraries=['rt'])
    ], compiler_directives={'language_level' : "3"}),
    zip_safe=False,
)