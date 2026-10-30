"""JSON-RPC envelopes, canonical-ID arguments and Unicode must retain identity."""
import json
import os
from pathlib import Path
import subprocess
import sys

binary = Path(sys.argv[1]).resolve(strict=True)
environment = dict(os.environ, LAPLACE_CONNINFO='host=127.0.0.1 port=1 dbname=unavailable connect_timeout=1')
requests = [
    {'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-06-18','capabilities':{},'clientInfo':{'name':'arguments-check','version':'1'}}},
    {'jsonrpc':'2.0','id':2,'method':'tools/call','params':{'name':'embeddings','arguments':{'input':'café 🐕'}}},
    {'jsonrpc':'2.0','id':'unicode-"direct','method':'tools/call','params':{'name':'embeddings','arguments':{'input':'café 🐕'}}},
    {'jsonrpc':'2.0','id':4,'method':'tools/call','params':{'name':'search','arguments':{'id':'f040f1c29f98d0f5d1bd7f265ac2e1de'}}},
    {'jsonrpc':'2.0','id':5,'method':'tools/call','params':{'name':'embeddings','arguments':{'input':'literal "id":99, {"input":"different"}\tend'}}},
    {'jsonrpc':'2.0','id':6,'method':'tools/call','params':{'name':'embeddings','arguments':{'input':'z'*256}}},
]
# The first representation escapes UTF-16 surrogate pairs, the second carries
# UTF-8 directly. Both go through the real MCP transport and Native decomposer.
wire='\n'.join(json.dumps(item,ensure_ascii=(index != 2)) for index,item in enumerate(requests))+'\n'
result=subprocess.run([str(binary),'mcp'],input=wire,env=environment,capture_output=True,text=True,encoding='utf-8',timeout=30)
assert result.returncode==0,result.stderr
responses=[json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
assert len(responses)==len(requests),result.stdout
assert [r['id'] for r in responses]==[r['id'] for r in requests]
assert responses[0]['result']['protocolVersion']=='2025-06-18'
def content(index):
    response=responses[index]['result']
    return response,json.loads(response['content'][0]['text'])
first,a=content(1);second,b=content(2)
assert not first['isError'] and not second['isError']
assert a['data']==b['data'],(a,b)
error,detail=content(3)
assert error['isError'] and detail['error']=='database',detail
last,literal=content(4)
assert not last['isError'] and literal!=a
bounded,long_word=content(5)
assert not bounded['isError'],long_word
record=long_word['data'][0]
assert record['tier']==2 and record['parts_total']==256 and not record['parts_complete'],record
assert len(record['parts'])==64 and len(set(record['parts']))==1,record
print('JSON-RPC IDs and arguments are separate; escaped/direct Unicode agree; canonical ID reaches the database reader; quoted field names stay content')
