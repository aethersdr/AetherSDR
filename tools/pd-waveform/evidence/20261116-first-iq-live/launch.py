import hashlib,json,os,pathlib,subprocess
p=pathlib.Path(__file__).resolve().parent;root=p.parent
assert subprocess.run(['pgrep','-x','AetherSDR'],capture_output=True).returncode!=0,'Existing instance; do not launch another'
profile=root/'restart-successor-live/private-profile';assert (profile/'AetherSDR.db').is_file()
binary=root/'first-install-desktop-monitor/project/AetherSDR'
env=os.environ.copy();env.pop('AETHER_AUTOMATION_ALLOW_TX',None);env.update(AETHER_SETTINGS_DIR=str(profile),AETHER_AUTOMATION='1',AETHER_AUTOMATION_NO_TX='1',AETHER_AUTOMATION_IDENTITY='aetherpd-shim-integration',AETHER_AUTOMATION_AGENT_NAME='Codex AetherPD',AETHER_AUTOMATION_SOCKET='aetherpd-first-iq-desktop')
with (p/'desktop.log').open('ab') as log:proc=subprocess.Popen([str(binary)],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
record={'pid':proc.pid,'socket':'/tmp/aetherpd-first-iq-desktop','binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'desktop_source':'fdf0a40861ddbc0a11308303656429fdfcbc9f78','package_source':'88280414febdaa69c42f4a7190a38b3f94b90238','automation_TX_disabled':True};(p/'instance.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))
