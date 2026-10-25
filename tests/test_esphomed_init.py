"""Execute shipped init start/readiness/status/stop using private paths only."""
import json,os,pathlib,socket,subprocess,tempfile,time
ROOT=pathlib.Path(__file__).resolve().parents[1]
BIN=os.environ['ESPHOMED_BIN']
with tempfile.TemporaryDirectory(prefix='esphomed-init-',dir=os.environ.get('TMPDIR')) as directory:
 p=pathlib.Path(directory);config=p/'config.json';config.write_text(json.dumps({'hostname':'fixture-satellite','wifi_mac':'02:00:00:00:00:01','voice_assistant_mode':1,'esphome_noise_key':''}));(p/'privacy').write_text('0\n')
 s=socket.socket();s.bind(('127.0.0.1',0));port=s.getsockname()[1];s.close()
 env=dict(os.environ,DEFAULTS=str(p/'missing-defaults'),DAEMON=BIN,PIDFILE=str(p/'pid'),LOGFILE=str(p/'log'),LE_CONFIG_PATH=str(config),STATUS_FILE=str(p/'status.json'),PORT=str(port),BIND='127.0.0.1',AUDIO_BUS=str(p/'system.pcm'),PRIVACY_STATE=str(p/'privacy'),IDME_ROOT=str(p/'idme'),TLS_CA=str(p/'missing-ca'))
 for k in ['AUDIO','WAKE','RADIO','TIMER','LED','MDNS']:env[k+'_SOCKET']=str(p/(k+'.sock'))
 script=ROOT/'init/libreecho-esphomed.init'
 def run(action):return subprocess.run(['sh',str(script),action],env=env,check=True,timeout=10,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 try:
  run('start');run('status');status=json.loads((p/'status.json').read_text());assert status['ready'] is True and status['connected'] is False
  pid=int((p/'pid').read_text());assert pathlib.Path('/proc',str(pid),'exe').resolve()==pathlib.Path(BIN).resolve();assert status['pid']==pid
  assert b'--stream-wav' not in pathlib.Path('/proc',str(pid),'cmdline').read_bytes()
  assert status['start_time']==pathlib.Path('/proc',str(pid),'stat').read_text().rsplit(')',1)[1].split()[19]
  assert status['listener_inode']!='0' and status['port']==port
  run('start');assert int((p/'pid').read_text())==pid  # ready restart is idempotent
 finally:run('stop')
 defaults=p/'esphomed-defaults';defaults.write_text('STREAM_WAV=1\n');env['DEFAULTS']=str(defaults)
 try:
  run('start');run('status');pid=int((p/'pid').read_text())
  assert b'--stream-wav\0' in pathlib.Path('/proc',str(pid),'cmdline').read_bytes()
 finally:run('stop')
 assert not (p/'pid').exists() and not (p/'status.json').exists()
 try:s=socket.create_connection(('127.0.0.1',port),.1)
 except ConnectionRefusedError:pass
 else:s.close();raise AssertionError('listener survived shipped stop')
print('ESPHome shipped init private-path start/readiness/status/stop cleanup: PASS')
# Watchdog and boot starts carry no LE_CONFIG_PATH. The default must then be
# the live user config that the web API writes (as for sttd/ttsd/agentd), not
# the read-only shipped seed, or HA ownership is unreadable after any restart.
config_line=next(l for l in (ROOT/'init/libreecho-esphomed.init').read_text().splitlines() if l.startswith('CONFIG='))
resolved=subprocess.run(['env','-i','PATH=/usr/bin:/bin','sh','-c',config_line+'; printf %s "$CONFIG"'],check=True,stdout=subprocess.PIPE,timeout=5).stdout.decode()
assert resolved=='/data/libreecho/config/web-config.json',resolved
resolved=subprocess.run(['env','-i','PATH=/usr/bin:/bin','LE_CONFIG_PATH=/private/override.json','sh','-c',config_line+'; printf %s "$CONFIG"'],check=True,stdout=subprocess.PIPE,timeout=5).stdout.decode()
assert resolved=='/private/override.json',resolved
print('ESPHome shipped init defaults to the live API config: PASS')
