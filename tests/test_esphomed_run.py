"""Focused build/exercise runner; never installs dependencies or touches hardware.
Example: python tests/test_esphomed_run.py --tls-prefix /path/to/mbedtls \
  --host-only --crypto-python /usr/bin/python3
Full acceptance: omit --host-only, give --aio-python an already-approved pinned
46.2.0 environment. --sanitize instruments all owned C with ASan/UBSan.
"""
import argparse,os,pathlib,subprocess,sys,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--tls-prefix',required=True);p.add_argument('--host-only',action='store_true');p.add_argument('--sanitize',action='store_true');p.add_argument('--crypto-python',default=sys.executable);p.add_argument('--aio-python',default=sys.executable);p.add_argument('--fixture-namespace',action='store_true',help=argparse.SUPPRESS);a=p.parse_args()
M=pathlib.Path(a.tls_prefix);scratch=pathlib.Path(os.environ.get('TMPDIR',ROOT/'build')).resolve();scratch.mkdir(parents=True,exist_ok=True)
# The real timerd regression must not reach host devices, runtime or network.
# Reuse only explicitly supplied private fixtures; standalone/CI entry wraps
# itself. In particular, never try nested bwrap under restrictive AppArmor.
if a.fixture_namespace or os.environ.get('ESPHOMED_PRIVATE_NAMESPACE')=='1':
 if list(pathlib.Path('/sys').iterdir()):raise SystemExit('Refusing unisolated ESPHome fixture: /sys is not private and empty')
 os.environ['ESPHOMED_PRIVATE_NAMESPACE']='1'
else:
 with tempfile.TemporaryDirectory(prefix='esphomed-namespace-',dir=scratch) as temporary:
  base=pathlib.Path(temporary)
  for name in ('tmp','run','data','sys','etc'):(base/name).mkdir()
  command=['bwrap','--unshare-user','--unshare-net','--unshare-pid','--unshare-ipc','--unshare-uts','--die-with-parent','--tmpfs','/']
  for entry in sorted(pathlib.Path('/').iterdir()):
   if entry.name in {'tmp','run','data','sys','etc','proc','dev'}:continue
   command+=['--symlink',os.readlink(entry),str(entry)] if entry.is_symlink() else ['--ro-bind',str(entry),str(entry)]
  command+=['--proc','/proc','--dev','/dev']
  for name in ('tmp','run','data','sys','etc'):command+=['--bind',str(base/name),'/'+name]
  for name in ('alternatives','ssl','passwd','group','nsswitch.conf','os-release','ld.so.cache','hosts'):
   source=pathlib.Path('/etc')/name
   if source.exists():command+=['--ro-bind',str(source),str(source)]
  command+=['--setenv','TMPDIR','/tmp','--setenv','PYTHONDONTWRITEBYTECODE','1','--setenv','ESPHOMED_PRIVATE_NAMESPACE','1','--chdir',str(ROOT),sys.executable,'-B',str(pathlib.Path(__file__).resolve()),*sys.argv[1:],'--fixture-namespace']
  raise SystemExit(subprocess.run(command,cwd=ROOT,timeout=480).returncode)
flags=[os.environ.get('CC','cc'),'-std=c99','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(M/'include')]
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
libs=[str(M/'lib'/('lib'+name+'.a')) for name in ['mbedtls','mbedx509','mbedcrypto']]+['-lm']
common=['src/adapter/esphome_proto.c','src/adapter/esphome_frame.c','src/adapter/esphome_noise.c','src/adapter/esphome_playback.c','src/adapter/radio_resample.c','src/adapter/mdns_client.c','src/adapter/mdns_lease.c','src/config_store.c','src/json.c']
def run(argv,env=None):
 print('+',' '.join(map(str,argv)),flush=True);subprocess.run(list(map(str,argv)),cwd=ROOT,env=env,check=True,timeout=90)
with tempfile.TemporaryDirectory(prefix='esphomed-suite-',dir=scratch) as directory:
 d=pathlib.Path(directory);daemon=d/'libreecho-esphomed';run(flags+['src/adapter/esphomed.c']+common+libs+['-o',daemon]);env=dict(os.environ,ESPHOMED_BIN=str(daemon));env['ASAN_OPTIONS']='detect_leaks=1';env['TMPDIR']=str(d);env['PYTHONDONTWRITEBYTECODE']='1'
 cases={
  'proto':['tests/test_esphome_proto.c','src/adapter/esphome_proto.c','src/adapter/esphome_frame.c'],
  'noise':['tests/test_esphomed_noise.c','src/adapter/esphome_noise.c'],
  'url':['tests/test_esphomed_playback.c','src/adapter/esphome_playback.c','src/adapter/radio_resample.c'],
  'http':['tests/test_esphomed_http.c','src/adapter/radio_resample.c'],
  'stream':['tests/test_esphomed_stream.c','src/adapter/radio_resample.c'],
  'pacing':['tests/test_esphomed_playback_pacing.c','src/adapter/radio_resample.c'],
  'queue':['tests/test_esphomed_queue.c']+common,
  'timer-lifecycle':['tests/test_esphomed_timer_lifecycle.c']+common,
  'idle':['tests/test_esphomed_idle.c']+common,
 }
 for name,sources in cases.items():
  binary=d/name;run(flags+sources+libs+['-o',binary])
  if name=='timer-lifecycle':
   for case in ['unsubscribe-full','disconnect-full','reconnect-full','reconnect-before-clear','sent-before-clear','sent-full','other-jobs-full','cleanup-retry','reconnect-adapter-capacity']:
    with tempfile.TemporaryDirectory(prefix='timer-',dir=d) as private:run([binary,case,private],env)
  elif name=='idle':
   for case in ['quiet-owner-pinged','silent-owner-dropped','idle-disconnect-no-error-flash','active-turn-disconnect-flashes']:
    with tempfile.TemporaryDirectory(prefix='idle-',dir=d) as private:run([binary,case,private],env)
  else:run([binary],env)
 run([sys.executable,'tests/test_esphome_ffmpeg_wav.py','--http-fixture',d/'http'],env)
 run([sys.executable,'tests/test_esphomed.py'],env)
 run([sys.executable,'tests/test_esphomed_native_lifecycle.py'],env)
 timerd=d/'libreecho-timerd';run(flags+['-Isrc','src/adapter/timerd.c','src/adapter/timer_schedule.c','src/adapter/adapter_client.c','src/adapter/adapter_server.c','src/json.c','src/log.c']+libs+['-o',timerd])
 run([sys.executable,'tests/test_esphomed_final_spec.py'],dict(env,TIMERD_BIN=str(timerd),ESPHOMED_PRIVATE_NAMESPACE='1'))
 run([a.crypto_python,'tests/test_esphomed_noise.py','NoiseFixture.test_noise_provision_persist_reconnect_wrong_key_and_plain_refusal','NoiseFixture.test_noise_replay_is_rejected','KeyedDaemon'],env)
 run([sys.executable,'tests/test_esphomed_tls.py','TLSFixture.test_https_validated_ca_hostname_wav','TLSFixture.test_https_untrusted_is_negative_completion','TLSFixture.test_https_trusted_wrong_hostname_is_negative_completion'],env)
 run([sys.executable,'tests/test_esphomed_watchdog.py'],env);run([sys.executable,'tests/test_esphomed_init.py'],env)
 run([sys.executable,'tests/test_esphome_health.py'],env)
 if not a.host_only:
  run([a.aio_python,'tests/test_esphomed_aio.py','PinnedFixture.test_pinned_plain_real_login_entities_ping_disconnect','PinnedFixture.test_pinned_noise_provision_reconnect_wrong_key'],env)
  run([a.aio_python,'tests/test_esphomed_media_state.py'],env)
  # Compare approved pinned serialization without overwriting checked-in evidence.
  run([a.aio_python,'tests/test_esphomed_generate_goldens.py','--check'],env)
 else:print('NOT RUN: pinned aioesphomeapi interoperability and pinned golden regeneration (--host-only)',flush=True)
print('Focused ESPHome fixtures and child/socket/file cleanup: PASS',flush=True)
