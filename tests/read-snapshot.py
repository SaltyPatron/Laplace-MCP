"""Persistent MCP requests refresh one shared read scope and retain idle connections."""
import contextlib
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import uuid


lpm,engine=(Path(value).resolve() for value in sys.argv[1:3])
pg_config=os.environ.get('PG_CONFIG')
psql=os.environ.get('PSQL') or (str(Path(pg_config).parent/'psql') if pg_config else shutil.which('psql') or 'psql')
host,port,user=(os.environ.get(key,default) for key,default in
                [('PGHOST','/tmp'),('PGPORT','5432'),('PGUSER','laplace')])
name='laplace_scope_'+uuid.uuid4().hex[:12]
connection=f'host={host} port={port} user={user} dbname={name}'
created=False


def sql(db,query):
    result=subprocess.run([psql,'-XwAtq','-h',host,'-p',port,'-U',user,'-d',db,
                           '-v','ON_ERROR_STOP=1','-c',query],capture_output=True,text=True,encoding='utf-8',timeout=180)
    if result.returncode:raise RuntimeError(result.stderr)
    return result.stdout.strip()


try:
    sql('postgres',f'CREATE DATABASE "{name}"');created=True
    sql(name,'CREATE EXTENSION laplace CASCADE')
    with contextlib.ExitStack() as resources:
        root=Path(resources.enter_context(tempfile.TemporaryDirectory(prefix='mcp-read-scope-',dir=os.environ.get('LAPLACE_WORK'))))
        recipes=root/'recipes';checkout=Path(__file__).resolve().parents[2]/'Laplace-Engine'
        shutil.copytree(checkout/'recipes',recipes)
        shutil.copytree(checkout/'firmware',root/'firmware')
        (root/'work').mkdir();(root/'code').mkdir()
        for source in ['first','second']:
            definitions=recipes/('scope-'+source);definitions.mkdir()
            (definitions/'source').write_text(f'name scope-{source}\nwitness Read scope {source}\nclass AcademicCurated\nroot $LAPLACE_DATA/{source}\n',encoding='utf-8',newline='\n')
            (definitions/'facts.recipe').write_text('name scope-'+source+'\nmatch *.tsv\ntier row by \\n\nnames thing value\ntier field by \\t\nthing row thing\ncontent thing value\nrelation "has value" value\nattest row value\n',encoding='utf-8',newline='\n')
            data=root/'data'/source;data.mkdir(parents=True)
            (data/'facts.tsv').write_text('dog\twarm\ncat\tcold\n',encoding='utf-8',newline='\n')
        env=dict(os.environ,LAPLACE_DATA=str(root/'data'),LAPLACE_WORK=str(root/'work'),
                 LAPLACE_RECIPES=str(recipes),LAPLACE_ENGINE=str(engine),LAPLACE_INGEST_ONE='1',OMP_NUM_THREADS='2')

        def ingest(source):
            result=subprocess.run([str(engine),'ingest','-r',str(recipes),'-d',connection,'-j','2',
                                   '-s','scope-'+source,str(root/'data'/source)],env=env,capture_output=True,
                                  text=True,encoding='utf-8',timeout=180)
            if result.returncode:raise RuntimeError(result.stdout+result.stderr)

        ingest('first')
        application='mcp_read_scope_'+uuid.uuid4().hex[:12]
        token=secrets.token_hex(32)
        server_env=dict(env,LAPLACE_CONNINFO=connection+' application_name='+application,
                        LAPLACE_MCP_TOKEN=token,LAPLACE_CODE_STORE=str(root/'code-store'),
                        LAPLACE_CODE_ROOTS=str(root/'code'),LAPLACE_CODE_SEMANTIC='')
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1',0));listen=reservation.getsockname()[1]
        log=resources.enter_context((root/'server.log').open('w+',encoding='utf-8'))
        process=subprocess.Popen([str(lpm),'serve','127.0.0.1',str(listen)],env=server_env,stdout=log,stderr=log)

        def stop():
            if process.poll() is None:process.terminate()
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait(timeout=10)
        resources.callback(stop)

        def request(path,data=None):
            req=urllib.request.Request(f'http://127.0.0.1:{listen}'+path,
                data=None if data is None else json.dumps(data).encode(),
                headers={'Content-Type':'application/json','Authorization':'Bearer '+token})
            with urllib.request.urlopen(req,timeout=90) as response:return json.load(response)

        deadline=time.monotonic()+15
        while True:
            try:
                assert request('/health')=={'ok':True};break
            except (OSError,TimeoutError):
                if process.poll() is not None or time.monotonic()>deadline:
                    log.flush();raise RuntimeError((root/'server.log').read_text(encoding='utf-8'))
                time.sleep(.1)

        def connections():
            rows=sql(name,f"SELECT pid,(xact_start IS NULL AND backend_xmin IS NULL)::int FROM pg_stat_activity WHERE application_name='{application}' ORDER BY pid")
            result={int(pid):int(idle) for pid,idle in (line.split('|') for line in rows.splitlines())}
            assert len(result)==1 and all(result.values()),result
            return set(result)

        # Stop this fixture's first namespace lookup. Its sole request connection
        # must already own a read snapshot before the indexed reads proceed.
        # The Engine lifecycle test independently proves writer isolation.
        gate=subprocess.Popen([psql,'-XwAtq','-h',host,'-p',port,'-U',user,'-d',name,
                               '-v','ON_ERROR_STOP=1'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,text=True,encoding='utf-8')
        def stop_gate():
            if gate.poll() is None:
                try:gate.communicate('ROLLBACK;\n',timeout=10)
                except subprocess.TimeoutExpired:gate.kill();gate.wait(timeout=10)
        resources.callback(stop_gate)
        gate.stdin.write("BEGIN; LOCK TABLE pg_catalog.pg_extension IN ACCESS EXCLUSIVE MODE; SELECT 'held';\n");gate.stdin.flush()
        assert gate.stdout.readline().strip()=='held'
        with ThreadPoolExecutor(max_workers=1) as executor:
            pending=executor.submit(request,'/v1/search',{'input':'dog'})
            try:
                deadline=time.monotonic()+15
                while True:
                    observed=sql(name,f"SELECT (backend_xmin IS NOT NULL AND xact_start IS NOT NULL)::int,coalesce((wait_event_type='Lock')::int,0) FROM pg_stat_activity WHERE application_name='{application}'")
                    rows=[line.split('|') for line in observed.splitlines()]
                    if any(row[1]=='1' for row in rows):
                        assert len(rows)==1 and rows[0][0]=='1',observed
                        break
                    if pending.done() or time.monotonic()>deadline:raise AssertionError('The request did not reach its owned read scope: '+observed)
                    time.sleep(.05)
            finally:
                stop_gate()
            first=pending.result(timeout=90)
        before={row['trunk'] for row in first['attestations'] if row['witness']}
        assert len(before)==1,first
        counts={row['entity']:row['matches'] for row in first['claims'] if row['stood']}
        assert counts and all(value>0 for value in counts.values()),first
        pids=connections()
        # The ID entry point discovers tiers and both constituent layers from
        # stored Native rows, without depending on text decomposition or leaf
        # names. The source trunk exercises nested, mixed-tier content.
        direct=request('/v1/search',{'id':first['id']})
        assert direct['physicality']==first['physicality'] and direct['claims']==first['claims'],direct
        source=request('/v1/search',{'id':next(iter(before))})
        assert source['physicality'] and source['constituents'],source
        assert any(part.get('constituents') for part in source['constituents']),source
        assert all(part['tier']>=0 for part in source['constituents']),source
        library=request('/v1/library',{})
        discovered={item['id']:item for item in library['items']}
        assert next(iter(before)) in discovered,library
        assert discovered[next(iter(before))]['kind']=='source',library
        assert discovered[next(iter(before))]['children'],library
        graph=request('/v1/provenance',{'id':first['id']})
        assert graph['complete'] and graph['edges'],graph
        assert before<={node['id'] for node in graph['nodes'] if node['metadata_trunk'] and node['root']},graph
        assert all('position' in edge and 'ordinal' in edge for edge in graph['edges']),graph
        assert connections()==pids
        ingest('second')
        second=request('/v1/search',{'input':'dog'})
        after={row['trunk'] for row in second['attestations'] if row['witness']}
        assert len(after)==2 and before<after,second
        assert any(row['matches']>counts[row['entity']] for row in second['claims'] if row['stood'] and row['entity'] in counts),second
        assert connections()==pids,'A new read should reuse the same connections and plans'
        # Exercise the PostgreSQL bigint wire contract beyond 32 bits inside
        # this private fixture, independently of evidence accumulation.
        claim=next(iter(counts));wide_count=4294967313
        sql(name,f"UPDATE public.consensus SET matches={wide_count} WHERE claim='{claim}'::public.blake3")
        third=request('/v1/search',{'input':'dog'})
        assert next(row['matches'] for row in third['claims'] if row['entity']==claim)==wide_count,third
        assert connections()==pids
        print('read-snapshot: one connection owns the snapshot and Native reads; source authority and 64-bit counts refresh between requests; all transactions released and connection PIDs reused')
finally:
    if created:sql('postgres',f'DROP DATABASE "{name}" WITH (FORCE)')
