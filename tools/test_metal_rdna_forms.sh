#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
cmake --build "$repo_dir/_Build/macos-arm64" --target kyty_emulator -j6
python3 - "$repo_dir" <<'PY'
from pathlib import Path
import subprocess,sys,tempfile
repo=Path(sys.argv[1]);build=repo/'_Build/macos-arm64';objects=build/'CMakeFiles/kyty_emulator.dir/src/graphics/shader/recompiler'
# Reuse the product's current compiler objects. The standalone program supplies
# assertion/log sinks and initializes only MTLDevice, without emulator startup.
files=[]
for p in [objects/'frontend',objects/'ir',objects/'backend/spirv',objects/'backend/msl']:
 files.extend(sorted(p.rglob('*.o')))
files.append(build/'CMakeFiles/kyty_emulator.dir/src/graphics/shader/shaderPixelParameter.cpp.o')
files.extend(build/'CMakeFiles/kyty_emulator.dir/src/graphics'/p for p in ['guest_gpu/gpu_format.cpp.o','host_gpu/renderer/cache/bufferCache.cpp.o'])
with tempfile.TemporaryDirectory(prefix='kytx-rdna-forms-') as tmp:
 executable=Path(tmp)/'metalRdnaFormsTests'
 cmd=['xcrun','clang++','-std=c++20','-fobjc-arc']
 for p in [repo/'src',repo/'tests',build,repo/'3rdparty/fmt/include',repo/'3rdparty/magic_enum/include/magic_enum',repo/'3rdparty/SPIRV-Headers/include',Path('/opt/homebrew/include')]:cmd+=['-I'+str(p)]
 cmd+=[str(repo/'tests/metalRdnaFormsTests.mm')]+list(map(str,files))
 cmd+=[str(build/'3rdparty/fmt/libfmt.a'),str(build/'3rdparty/SPIRV-Tools/source/opt/libSPIRV-Tools-opt.a'),str(build/'3rdparty/SPIRV-Tools/source/libSPIRV-Tools.a'),'-L/opt/homebrew/lib','-lspirv-cross-msl','-lspirv-cross-glsl','-lspirv-cross-core','-Wl,-dead_strip','-framework','Metal','-framework','Foundation','-o',str(executable)]
 subprocess.run(cmd,check=True);subprocess.run([str(executable)],check=True)
PY
