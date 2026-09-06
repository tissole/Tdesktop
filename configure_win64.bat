@echo OFF
REM Configure from the repository root instead of cmake/run_cmake.py,
REM so the cmake helpers submodule stays pristine. VS2026 toolset.
cmake -S . -B out -Ax64 -T v145 -DCMAKE_BUILD_TYPE=Debug -Werror=dev -Werror=deprecated --warn-uninitialized
