#!/usr/bin/env python3
"""Fixed service metadata, installed definitions and command-admission tests."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest
import units
CHECKER=None
NAMES=['aetherpd-boot.service','aetherpd-start.socket','aetherpd-shim.service']
BOOT='/var/lib/smartsdr/aetherpd/pd-boot'
def executable(path,arg=''):
 return '{ path='+path+' ; argv[]='+path+(' '+arg if arg else '')+' ; ignore_errors=no ; start_time=[n/a] ; stop_time=[n/a] ; pid=0 ; code=(null) ; status=0/0 }'
def properties(kind,active=False):
 name=NAMES[kind]
 d=dict(Id=name,Names=name,LoadState='loaded',ActiveState='active' if active else 'inactive',SubState=('listening' if kind==1 else 'exited' if kind==0 else 'running') if active else 'dead',FragmentPath='/etc/systemd/system/'+name,SourcePath='',DropInPaths='',NeedDaemonReload='no',Transient='no')
 if kind==1:d.update(Listen='/run/aetherpd/start.sock (Stream)',Accept='yes',ExecStartPost=executable(BOOT,'--socket-owner'))
 else:d.update(MainPID='42' if active and kind==2 else '0',ControlPID='0',ControlGroup='/system.slice/'+name if active else '',Type='oneshot' if kind==0 else 'simple',ExecStart=executable(BOOT if kind==0 else '/run/aetherpd/bin/pd-primary-launch'),KillMode='control-group',RemainAfterExit='yes' if kind==0 else 'no',StandardInput='null')
 return d
def text(values):return ''.join(k+'='+v+'\n' for k,v in values.items())
class ServiceTest(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory(prefix='pd-service-');self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
  self.units=self.root/'etc/systemd/system';self.units.mkdir(parents=True)
  for name,value in units.render('arm64').items():(self.units/name).write_text(value);(self.units/name).chmod(0o644)
  for i,n in enumerate(NAMES):(self.root/n).write_text(text(properties(i)))
  (self.root/'sessions').write_text('');(self.root/'proc').mkdir()
  (self.root/'aetherpd-start@0-2-78666.service').write_text(text(self.session('aetherpd-start@0-2-78666.service')))
 def invoke(self,mode,kind=None,data=None,expected=0,pid=0,active=0,quiet=0):
  if mode in ('parse','worker-parse','upgrade-worker-parse'):
   p=self.root/'input';p.write_bytes(data.encode() if isinstance(data,str) else data);args=[mode,str(kind),str(p)]
  else:args=[mode,str(self.root)]+([] if kind is None else [str(kind)])
  p=subprocess.run([str(CHECKER),*args],capture_output=True,timeout=5)
  self.assertEqual(p.returncode,expected,p.stderr.decode(errors='replace'))
  self.assertEqual(p.stdout,('ok %d %d %d\n'%(pid,active,quiet)).encode() if expected==0 else b'')
 def parse(self,kind,values,expected=0,**kw):self.invoke('parse',kind,text(values),expected,**kw)
 def worker(self,recovery):
  d=properties(2);name='aetherpd-recover.service' if recovery else 'aetherpd-install.service'
  d.update(Id=name,Names=name,FragmentPath='/etc/systemd/system/'+name,Type='oneshot',ExecStart=executable(BOOT,'--recover-installation' if recovery else '--install-approved'))
  return d
 def layout(self,managed,hold=True):
  for name,value in units.render('arm64',managed,hold).items():(self.units/name).write_text(value)
  d=properties(2)
  if managed:d['ExecStart']=executable(BOOT,'--primary')
  (self.root/NAMES[2]).write_text(text(d))
  d=self.session('aetherpd-start@0-2-78666.service')
  d['SendSIGKILL']='no' if hold else 'yes'
  if managed:d['ExecStart']=executable(BOOT,'--session')
  (self.root/d['Id']).write_text(text(d))
 def test_both_complete_layouts_reload_stop_and_quiesce(self):
  for managed in (False,True):
   self.layout(managed)
   self.invoke('files');self.invoke('reload');self.invoke('read',2,quiet=1)
   self.invoke('stop');self.invoke('quiesce')
 def test_upgrade_cleanup_guard_requires_completed_boot_and_socket_owner(self):
  for kind in (0,1):
   d=properties(kind,True);d['ControlPID']='0';(self.root/NAMES[kind]).write_text(text(d))
  self.invoke('upgrade-cleanup-idle')
  for kind,changes in [(0,{'ActiveState':'activating','SubState':'start','MainPID':'42'}),
                       (0,{'ControlPID':'42'}),(0,{'ActiveState':'inactive','SubState':'dead'}),
                       (1,{'ControlPID':'42'}),(1,{'ActiveState':'activating','SubState':'start-post'}),
                       (1,{'ActiveState':'inactive','SubState':'dead'})]:
   d=properties(kind,True);d['ControlPID']='0';d.update(changes)
   (self.root/NAMES[kind]).write_text(text(d));self.invoke('upgrade-cleanup-idle',expected=1)
   d=properties(kind,True);d['ControlPID']='0';(self.root/NAMES[kind]).write_text(text(d))
  self.invoke('upgrade-cleanup-idle')
  self.assertTrue(all('systemctl show' in line for line in (self.root/'commands').read_text().splitlines()))
 def test_upgrade_cleanup_guard_requires_socket_control_pid_evidence(self):
  (self.root/NAMES[0]).write_text(text(properties(0,True)))
  for value in [None,'','-1','1','00','not-a-pid']:
   d=properties(1,True)
   if value is not None:d['ControlPID']=value
   (self.root/NAMES[1]).write_text(text(d));self.invoke('upgrade-cleanup-idle',expected=1)
 def test_upgrade_cleanup_guard_rejects_foreign_installed_or_loaded_units(self):
  for kind in (0,1):
   d=properties(kind,True);d['ControlPID']='0';(self.root/NAMES[kind]).write_text(text(d))
  self.invoke('upgrade-cleanup-idle')
  d=properties(1,True);d.update(ControlPID='0',DropInPaths='/foreign.conf')
  (self.root/NAMES[1]).write_text(text(d));self.invoke('upgrade-cleanup-idle',expected=1)
  (self.units/NAMES[0]).write_text('foreign');(self.root/'commands').unlink()
  self.invoke('upgrade-cleanup-idle',expected=1);self.assertFalse((self.root/'commands').exists())
 def test_mixed_installed_layout_refuses_before_commands(self):
  for managed in (False,True):
   self.layout(managed)
   (self.units/NAMES[2]).write_text(units.render('arm64',not managed)[NAMES[2]])
   (self.root/'commands').unlink(missing_ok=True)
   for mode in ('files','reload','quiesce','stop'):self.invoke(mode,expected=1)
   self.assertFalse((self.root/'commands').exists())
 def test_loaded_layout_must_match_files_before_stop(self):
  for managed in (False,True):
   for name,arg in [(NAMES[2],'--primary'),('aetherpd-start@0-2-78666.service','--session')]:
    self.layout(managed)
    d=properties(2) if arg=='--primary' else self.session(name)
    if not managed:d['ExecStart']=executable(BOOT,arg)
    (self.root/name).write_text(text(d));(self.root/'commands').unlink(missing_ok=True)
    self.invoke('quiesce',expected=1)
    self.assertNotIn('systemctl stop',(self.root/'commands').read_text())
 def test_live_session_layout_must_match_template_before_stop(self):
  for managed in (False,True):
   self.layout(managed);d=self.session()
   if not managed:d['ExecStart']=executable(BOOT,'--session')
   (self.root/'sessions').write_text(text(d));(self.root/'commands').unlink(missing_ok=True)
   self.invoke('quiesce',expected=1)
   self.assertNotIn('systemctl stop',(self.root/'commands').read_text())
 def test_managed_entry_exact_arguments(self):
  for arg in ['--session','--primary extra','--primary --session','--install-approved','']:
   d=properties(2);d['ExecStart']=executable(BOOT,arg);self.parse(2,d,1)
  d=self.session();d['ExecStart']=executable(BOOT,'--primary');self.raw('sessions',text(d),1)
 def test_mixed_session_inventory_refused(self):
  a=self.session();b=self.session('aetherpd-start@1-1193-78666.service')
  b['ExecStart']=executable(BOOT,'--session')
  self.raw('sessions',text(a)+'\n'+text(b),1)
 def test_worker_exact_mode_and_no_extra_authority(self):
  for recovery in (0,1):
   d=self.worker(recovery);self.invoke('worker-parse',recovery,text(d),quiet=1)
   for field,value in [('ExecStart',executable(BOOT,'--install-approved' if recovery else '--recover-installation')),('ExecStart',executable(BOOT,'--recover-installation extra')),('DropInPaths','/tmp/override'),('Environment','EVIL=1'),('ExecStartPre','unexpected'),('KillMode','process'),('Type','simple'),('RemainAfterExit','yes')]:
    bad=dict(d);bad[field]=value;self.invoke('worker-parse',recovery,text(bad),expected=1)
 def test_worker_activating_main_pid_and_cgroup(self):
  for recovery in (0,1):
   d=self.worker(recovery);d.update(ActiveState='activating',SubState='start',MainPID='42',ControlGroup='/system.slice/'+d['Id'])
   self.invoke('worker-parse',recovery,text(d),pid=42)
   d['ControlGroup']='/system.slice/foreign.service';self.invoke('worker-parse',recovery,text(d),expected=1)
 def test_worker_parser_rejects_other_selectors(self):
  for recovery in (-1,2,3):self.invoke('worker-parse',recovery,text(self.worker(0)),expected=1)
 def upgrade_worker(self,recovery):
  d=self.worker(recovery);name='aetherpd-upgrade-recover.service' if recovery else 'aetherpd-upgrade.service'
  d.update(Id=name,Names=name,FragmentPath='/etc/systemd/system/'+name,ExecStart=executable('/var/lib/smartsdr/aetherpd-upgrade/worker','--recover' if recovery else ''))
  return d
 def upgrade_units(self):
  import upgrade_units
  for name,value in upgrade_units.render('arm64').items():(self.units/name).write_text(value);(self.units/name).chmod(0o644)
  wants=self.units/'multi-user.target.wants';wants.mkdir();(wants/'aetherpd-upgrade-recover.service').symlink_to('../aetherpd-upgrade-recover.service')
  for recovery in (0,1):
   d=self.upgrade_worker(recovery);(self.root/d['Id']).write_text(text(d))
 def test_upgrade_worker_exact_path_and_mode(self):
  for recovery in (0,1):
   d=self.upgrade_worker(recovery);self.invoke('upgrade-worker-parse',recovery,text(d),quiet=1)
   for field,value in [('ExecStart',executable(BOOT,'--recover-installation')),('ExecStart',executable('/var/lib/smartsdr/aetherpd-upgrade/worker','--bootstrap')),('DropInPaths','/tmp/foreign'),('Environment','X=1'),('ExecStop','foreign'),('KillMode','process')]:
    bad=dict(d);bad[field]=value;self.invoke('upgrade-worker-parse',recovery,text(bad),expected=1)
 def test_upgrade_worker_self_and_other_unit_quiet(self):
  self.upgrade_units()
  for recovery in (0,1):
   d=self.upgrade_worker(recovery);self.invoke('upgrade-worker-ready',recovery,expected=1)
   d.update(ActiveState='activating',SubState='start',MainPID='@self',ControlGroup='/system.slice/'+d['Id'])
   (self.root/d['Id']).write_text(text(d));self.invoke('upgrade-worker-ready',recovery)
   d['MainPID']='42';(self.root/d['Id']).write_text(text(d));self.invoke('upgrade-worker-ready',recovery,expected=1)
   d['MainPID']='@self';(self.root/d['Id']).write_text(text(d))
   other=self.upgrade_worker(1-recovery);other.update(ActiveState='activating',SubState='start',MainPID='43',ControlGroup='/system.slice/'+other['Id'])
   (self.root/other['Id']).write_text(text(other));self.invoke('upgrade-worker-ready',recovery,expected=1)
   for i in (0,1):
    d=self.upgrade_worker(i);(self.root/d['Id']).write_text(text(d))
 def test_upgrade_worker_launch_only_when_both_quiet(self):
  self.upgrade_units();self.invoke('upgrade-worker-launch',0)
  self.assertIn('/usr/bin/systemctl start --no-block aetherpd-upgrade.service',(self.root/'commands').read_text())
  (self.root/'commands').unlink()
  d=self.upgrade_worker(1);d.update(ActiveState='activating',SubState='start',MainPID='43',ControlGroup='/system.slice/'+d['Id'])
  (self.root/d['Id']).write_text(text(d));self.invoke('upgrade-worker-launch',0,expected=1)
  self.assertNotIn('systemctl start',(self.root/'commands').read_text())
 def test_upgrade_worker_launch_accepts_interrupted_runtime_definitions(self):
  self.upgrade_units()
  (self.units/'aetherpd-shim.service').write_text(units.render('arm64',True)['aetherpd-shim.service'])
  self.invoke('upgrade-worker-launch',0)
  self.assertIn('/usr/bin/systemctl start --no-block aetherpd-upgrade.service',(self.root/'commands').read_text())
 def test_primary_autostart_enable_disable_and_repeat(self):
  link=self.units/'multi-user.target.wants/aetherpd-shim.service'
  self.invoke('autostart',0);self.invoke('autostart',1);self.assertEqual(str(link.readlink()),'../aetherpd-shim.service')
  self.invoke('autostart',1);self.invoke('autostart',0);self.assertFalse(link.is_symlink());self.invoke('autostart',0)
 def test_primary_autostart_preserves_foreign_links(self):
  wants=self.units/'multi-user.target.wants';wants.mkdir()
  for name in ['aetherpd-shim.service','.aetherpd-shim.service.new']:
   p=wants/name;p.symlink_to('../foreign.service')
   for enabled in (0,1):self.invoke('autostart',enabled,expected=1);self.assertEqual(str(p.readlink()),'../foreign.service')
   p.unlink()
 def test_primary_autostart_resumes_published_partial(self):
  wants=self.units/'multi-user.target.wants';wants.mkdir();partial=wants/'.aetherpd-shim.service.new';partial.symlink_to('../aetherpd-shim.service')
  self.invoke('autostart',1);self.assertFalse(partial.is_symlink());self.invoke('autostart',0)
 def test_primary_autostart_refuses_definition_change(self):
  (self.units/NAMES[2]).write_text('foreign');self.invoke('autostart',1,expected=1)
  self.assertFalse((self.units/'multi-user.target.wants').exists())
 def test_fixed_active_and_inactive_service_shapes(self):
  for i in range(3):
   self.parse(i,properties(i),quiet=1)
   self.parse(i,properties(i,True),pid=42 if i==2 else 0,active=1)
 def test_override_alias_shadow_and_stale_loaded_definitions_refused(self):
  for field,value in [('Id','other.service'),('Names',NAMES[2]+' other.service'),('LoadState','masked'),('FragmentPath','/run/systemd/system/'+NAMES[2]),('SourcePath','/tmp/input'),('DropInPaths','/etc/systemd/system/service.d/x.conf'),('NeedDaemonReload','yes'),('Transient','yes')]:
   d=properties(2);d[field]=value;self.parse(2,d,1)
 def test_environment_and_auxiliary_commands_refused(self):
  for field in ['User','Group','Environment','EnvironmentFiles','WorkingDirectory','RootDirectory','ExecStartPre','ExecStartPost','ExecStop','ExecStopPost','ExecReload']:
   d=properties(2);d[field]='unexpected';self.parse(2,d,1)
 def test_exec_exact_path_arguments_and_single_command(self):
  for i in range(3):
   field='ExecStartPost' if i==1 else 'ExecStart';good=properties(i)[field]
   for bad in [good.replace('path=/','path=/evil/',1),good.replace(' ; ignore_errors',' extra ; ignore_errors',1),good.replace('ignore_errors=no','ignore_errors=yes'),good+' '+good,good.replace('status=0/0 }','status=0/0 ; extra=yes }'),good[:-1],good.replace('pid=0','pid=-1')]:
    d=properties(i);d[field]=bad;self.parse(i,d,1)
 def test_missing_duplicate_unknown_and_truncated_properties(self):
  d=properties(2)
  for field in d:
   v=dict(d);del v[field];self.parse(2,v,1)
   self.invoke('parse',2,text(d)+field+'='+d[field]+'\n',1)
  for data in [b'',text(d)[:-1],text(d)+'Unknown=1\n',text(d)+'\n',text(d)+'\0',b'x'*16384]:self.invoke('parse',2,data,1)
 def test_optional_empty_arrays_are_accepted(self):
  d=properties(2)
  for field in ['ExecStartPre','ExecStop','ExecStopPost','ExecReload','EnvironmentFiles']:d[field]=''
  self.parse(2,d,quiet=1)
 def test_pid_state_cgroup_and_type_conflicts(self):
  for field,value in [('MainPID','1'),('MainPID','042'),('MainPID','-42'),('MainPID','4194305'),('MainPID','0'),('ControlPID','43'),('ControlGroup','/system.slice/other.service'),('ControlGroup',''),('Type','forking'),('SubState','dead'),('KillMode','process'),('RemainAfterExit','yes'),('StandardInput','socket')]:
   d=properties(2,True);d[field]=value;self.parse(2,d,1)
  d=properties(2);d['MainPID']='42';self.parse(2,d,1)
 def test_socket_address_accept_and_post_command(self):
  for field,value in [('Listen','/tmp/start.sock (Stream)'),('Listen','/run/aetherpd/start.sock (Stream) /tmp/other (Stream)'),('Accept','no'),('ExecStartPost',''),('ExecStart','unexpected')]:
   d=properties(1);d[field]=value;self.parse(1,d,1)
 def test_installed_files_all_required(self):
  self.invoke('files')
  for name in units.render('arm64'):
   p=self.units/name;data=p.read_bytes();p.unlink();self.invoke('files',expected=1);p.write_bytes(data);p.chmod(0o644)
 def test_file_content_mode_symlink_hardlink_and_parent_refuse(self):
  p=self.units/NAMES[2];data=p.read_bytes();p.write_bytes(data+b'\n');self.invoke('files',expected=1);p.write_bytes(b'X'+data[1:]);self.invoke('files',expected=1);p.write_bytes(data)
  p.chmod(0o666);self.invoke('files',expected=1);p.chmod(0o644)
  link=self.root/'extra';link.hardlink_to(p);self.invoke('files',expected=1);link.unlink()
  p.unlink();target=self.root/'target';target.write_bytes(data);p.symlink_to(target);self.invoke('files',expected=1);p.unlink();p.write_bytes(data);p.chmod(0o644)
  self.units.chmod(0o777);self.invoke('files',expected=1)
 def test_reload_validates_disk_before_any_command(self):
  (self.units/NAMES[2]).write_text('foreign');self.invoke('reload',expected=1);self.assertFalse((self.root/'commands').exists())
 def test_reload_and_read_use_only_fixed_commands(self):
  self.invoke('reload');log=(self.root/'commands').read_text().splitlines();self.assertEqual(log[0],'/usr/bin/systemctl daemon-reload');self.assertEqual(len(log),5)
  for i,name in enumerate(NAMES):self.assertTrue(log[i+1].startswith('/usr/bin/systemctl show --all '+name+' --property='))
  self.invoke('read',2,quiet=1)
 def test_loaded_override_refuses_after_reload(self):
  d=properties(2);d['DropInPaths']='/etc/systemd/system/aetherpd-shim.service.d/foreign.conf';(self.root/NAMES[2]).write_text(text(d));self.invoke('reload',expected=1)
 def test_template_override_refuses_without_live_sessions(self):
  d=self.session('aetherpd-start@0-2-78666.service');d['DropInPaths']='/etc/systemd/system/aetherpd-start@.service.d/foreign.conf'
  (self.root/d['Id']).write_text(text(d));self.invoke('reload',expected=1)
  self.invoke('quiesce',expected=1);self.assertNotIn('systemctl stop',(self.root/'commands').read_text())
 def test_failed_command_refuses(self):
  (self.root/'command-fail').touch();self.invoke('reload',expected=1)
 def test_stop_checks_definition_and_quiet_postcondition(self):
  self.invoke('stop');self.assertIn('/usr/bin/systemctl stop aetherpd-shim.service\n',(self.root/'commands').read_text())
  (self.root/NAMES[2]).write_text(text(properties(2,True)));self.invoke('stop',expected=1)
  d=properties(2);d['ExecStop']='foreign';(self.root/NAMES[2]).write_text(text(d));(self.root/'commands').unlink();self.invoke('stop',expected=1)
  self.assertNotIn('systemctl stop',(self.root/'commands').read_text())
 def session(self,name='aetherpd-start@0-1193-78666.service',active=False):
  d=properties(2,active);d.update(Id=name,Names=name,FragmentPath='/etc/systemd/system/aetherpd-start@.service',ExecStart=executable('/run/aetherpd/bin/pd-start-session'),StandardInput='socket',SendSIGKILL='no',ControlGroup=('/system.slice/system-aetherpd\\x2dstart.slice/'+name) if active else '')
  return d
 def test_applied_session_hold_is_required_for_startup(self):
  for managed in (False,True):
   self.layout(managed)
   current=self.session(active=True);current['MainPID']='@self'
   if managed:current['ExecStart']=executable(BOOT,'--session')
   (self.root/'sessions').write_text(text(current));self.invoke('session-policy')
   name='aetherpd-start@0-2-78666.service'
   for value in [None,'yes','','NO','0','no no']:
    d=self.session(name)
    if managed:d['ExecStart']=executable(BOOT,'--session')
    if value is None:d.pop('SendSIGKILL')
    else:d['SendSIGKILL']=value
    (self.root/name).write_text(text(d));(self.root/'commands').unlink(missing_ok=True)
    self.invoke('session-policy',expected=1)
    self.assertNotIn('systemctl start',(self.root/'commands').read_text())
 def test_startup_requires_own_live_applied_session_policy(self):
  self.layout(True)
  self.invoke('session-policy',expected=1)
  current=self.session(active=True);current.update(MainPID='@self',ExecStart=executable(BOOT,'--session'))
  (self.root/'sessions').write_text(text(current));self.invoke('session-policy')
  for changes in [{'MainPID':'42'},{'SendSIGKILL':'yes'},{'MainPID':'0','ControlGroup':'','ActiveState':'inactive','SubState':'dead'}]:
   changed=dict(current);changed.update(changes)
   (self.root/'sessions').write_text(text(changed));self.invoke('session-policy',expected=1)
 def test_legacy_stop_policy_is_quiesce_only(self):
  for managed in (False,True):
   self.layout(managed,False);self.invoke('files');self.invoke('reload')
   self.invoke('session-policy',expected=1);self.invoke('quiesce')
   for active,populated in [(True,True),(False,True)]:
    d=self.session(active=active);d['SendSIGKILL']='yes'
    if managed:d['ExecStart']=executable(BOOT,'--session')
    if populated:d['ControlGroup']='/system.slice/system-aetherpd\\x2dstart.slice/'+d['Id']
    (self.root/'sessions').write_text(text(d));(self.root/'commands').unlink(missing_ok=True)
    self.invoke('quiesce',expected=1)
    self.assertNotIn('systemctl stop',(self.root/'commands').read_text())
   (self.root/'sessions').write_text('')
 def test_legacy_hidden_kernel_session_refuses_before_stop(self):
  self.layout(True,False)
  self.process(42,'0::/system.slice/system-aetherpd\\x2dstart.slice/aetherpd-start@0-1193-78666.service\n','/run/aetherpd/bin/pd-lease-guard')
  self.invoke('quiesce',expected=1)
  self.assertNotIn('systemctl stop',(self.root/'commands').read_text())
 def test_legacy_orphan_runtime_helper_refuses_before_stop(self):
  self.layout(True,False)
  self.process(42,'0::/system.slice/foreign.service\n','/run/aetherpd/bin/pd-lease-guard')
  self.invoke('quiesce',expected=1)
  self.assertNotIn('systemctl stop',(self.root/'commands').read_text())
 def test_legacy_primary_control_may_retire_normally(self):
  self.layout(True,False)
  self.process(42,'0::/system.slice/aetherpd-shim.service\n','/run/aetherpd/bin/flex-tailnet-shim')
  self.invoke('quiesce',expected=1)
  self.assertIn('systemctl stop aetherpd-shim.service aetherpd-start.socket',(self.root/'commands').read_text())
 def test_applied_policy_must_match_fixed_template_and_all_instances(self):
  for managed in (False,True):
   self.layout(managed)
   d=self.session();d['SendSIGKILL']='yes'
   if managed:d['ExecStart']=executable(BOOT,'--session')
   (self.root/'sessions').write_text(text(d));(self.root/'commands').unlink(missing_ok=True)
   self.invoke('quiesce',expected=1)
   self.assertNotIn('systemctl stop',(self.root/'commands').read_text())
   (self.root/'sessions').write_text('')
 def test_session_policy_missing_duplicate_or_invalid_refused(self):
  d=self.session()
  for value in [None,'','NO','false','0']:
   changed=dict(d)
   if value is None:changed.pop('SendSIGKILL')
   else:changed['SendSIGKILL']=value
   self.raw('sessions',text(changed),1)
  self.raw('sessions',text(d)+'SendSIGKILL=no\n',1)
  legacy=dict(d);legacy['Id']=legacy['Names']='aetherpd-start@1-1193-78666.service';legacy['SendSIGKILL']='yes'
  self.raw('sessions',text(d)+'\n'+text(legacy),1)
 def raw(self,mode,data,expected=0,extra=''):
  path=self.root/'input';path.write_bytes(data.encode() if isinstance(data,str) else data)
  p=subprocess.run([str(CHECKER),mode,str(path)],capture_output=True,timeout=5)
  self.assertEqual(p.returncode,expected,p.stderr.decode(errors='replace'))
  self.assertEqual(p.stdout,(extra+'ok 0 0 0\n').encode() if expected==0 else b'')
 def test_empty_and_multiple_session_inventory(self):
  self.raw('sessions','')
  a=self.session();b=self.session('aetherpd-start@1-1193-78666.service',True)
  self.raw('sessions',text(a)+'\n'+text(b)+'\n',extra=a['Id']+' 0 0 1\n'+b['Id']+' 42 1 0\n')
 def test_session_name_grammar_and_duplicates(self):
  for name in ['aetherpd-start@.service','other@0-1193-78666.service','aetherpd-start@0-1193-0.service','aetherpd-start@00-1193-78666.service','aetherpd-start@18446744073709551616-1193-78666.service','aetherpd-start@0-1-78666.service','aetherpd-start@0-1193-78666.service/other','aetherpd-start@0-1193-78666.service x.service']:
   self.raw('sessions',text(self.session(name)),1)
  block=text(self.session());self.raw('sessions',block+'\n'+block,1)
 def test_session_overrides_aliases_wrong_cgroup_and_command_refuse(self):
  for field,value in [('DropInPaths','/tmp/override'),('Names','other.service'),('FragmentPath','/etc/systemd/system/aetherpd-start@0-1193-78666.service'),('ControlGroup','/system.slice/other.service'),('ExecStart',executable('/usr/bin/false')),('StandardInput','null'),('KillMode','process')]:
   d=self.session(active=True);d[field]=value;self.raw('sessions',text(d),1)
 def test_session_inventory_boundaries(self):
  for value in ['\n',text(self.session())[:-1],text(self.session())+'\n\n',b'x'*65536]:self.raw('sessions',value,1)
  blocks=[text(self.session('aetherpd-start@%d-1193-78666.service'%i)) for i in range(33)]
  self.raw('sessions','\n'.join(blocks),1)
 def test_owned_cgroup_exact_boundaries_and_descendants(self):
  for name in NAMES:
   for suffix in ['', '/child']:
    self.raw('cgroup','0::/system.slice/'+name+suffix+'\n',extra='owned 1\n')
  path='/system.slice/system-aetherpd\\x2dstart.slice/aetherpd-start@0-1193-78666.service'
  self.raw('cgroup','0::'+path+'\n',extra='owned 1\n')
  self.raw('cgroup','2:cpu:/\n1:name=systemd:'+path+'/child\n',extra='owned 1\n')
  for value in ['0::/system.slice/unrelated.service\n','0::/system.slice/aetherpd-shim.service-unrelated\n','0::/system.slice/system-aetherpd\\x2dstart.slice/other.service\n']:
   self.raw('cgroup',value,extra='owned 0\n')
 def test_cgroup_malformed_and_truncated_refused(self):
  for value in ['', 'broken\n','0::relative\n','x::/\n','0::/','0::/\n\n',b'x'*8192]:self.raw('cgroup',value,1)
 def process(self,pid,group,exe=None):
  p=self.root/'proc'/str(pid);p.mkdir();(p/'cgroup').write_text(group)
  if exe:(p/'exe').symlink_to(exe)
 def test_runtime_quiet_checks_both_cgroup_and_executable(self):
  self.invoke('quiet');self.process(42,'0::/system.slice/other.service\n','/usr/bin/ordinary');self.invoke('quiet')
  p=self.root/'proc/42/exe';p.unlink();p.symlink_to('/run/aetherpd-unrelated/bin/app');self.invoke('quiet')
  p.unlink();p.symlink_to('/run/aetherpd/bin/aether-pd (deleted)');self.invoke('quiet',expected=1)
  p.unlink();(self.root/'proc/42/cgroup').write_text('0::/system.slice/aetherpd-shim.service\n');self.invoke('quiet',expected=1)
 def test_proc_unreadable_or_malformed_refuses(self):
  self.process(42,'invalid\n');self.invoke('quiet',expected=1)
 def test_quiesce_stops_only_verified_exact_names(self):
  a=self.session();(self.root/'sessions').write_text(text(a));self.invoke('quiesce')
  commands=(self.root/'commands').read_text().splitlines();stops=[v for v in commands if 'systemctl stop' in v]
  self.assertEqual(stops,['/usr/bin/systemctl stop aetherpd-shim.service aetherpd-start.socket','/usr/bin/systemctl stop '+a['Id'],'/usr/bin/systemctl stop aetherpd-boot.service'])
 def test_foreign_session_refuses_before_any_stop(self):
  d=self.session();d['ExecStop']='foreign';(self.root/'sessions').write_text(text(d));self.invoke('quiesce',expected=1)
  self.assertNotIn('systemctl stop',(self.root/'commands').read_text())
 def test_live_session_or_survivor_prevents_quiescence_success(self):
  (self.root/'sessions').write_text(text(self.session(active=True)));self.invoke('quiesce',expected=1)
  (self.root/'sessions').write_text('');self.process(42,'0::/system.slice/other.service\n','/run/aetherpd/bin/pd-fixed-arp');self.invoke('quiesce',expected=1)
 def test_intake_still_active_prevents_session_stop(self):
  (self.root/NAMES[1]).write_text(text(properties(1,True)));(self.root/'sessions').write_text(text(self.session()));self.invoke('quiesce',expected=1)
  stops=[s for s in (self.root/'commands').read_text().splitlines() if 'systemctl stop' in s]
  self.assertEqual(stops,['/usr/bin/systemctl stop aetherpd-shim.service aetherpd-start.socket'])
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True);a,rest=p.parse_known_args();CHECKER=a.checker.resolve();unittest.main(argv=[__file__,*rest])
