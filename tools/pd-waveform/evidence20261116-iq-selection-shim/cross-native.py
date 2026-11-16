import importlib.util,concurrent.futures
from pathlib import Path
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source');out=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('pd_build',root/'tools/pd-waveform/build.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def build(arch):
 m.zig_cc(m.ARMV7 if arch=='armv7' else m.ARM64,out/('aether-pd-'+arch),out/('native-cache-'+arch))
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:list(pool.map(build,['armv7','arm64']))
