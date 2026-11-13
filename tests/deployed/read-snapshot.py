"""A persistent lpm serve reads the deployed database in one shared read scope, refreshes it between requests, and keeps
its one idle connection: what it read before a source was ingested and after, through the same connection.

The check's first source holds two TSV facts and an XML and a JSON file that reference the same word, its second the
same TSV facts again; both under witnesses of the check ("Laplace check read-snapshot first|second"). Its words are
built at run time from a prefix nothing else in the database holds, so every claim, trunk and physicality the reads
return for them is the check's. lpm serve listens on a free loopback port with the check's own token, its sessions
named for pg_stat_activity by an application name of the check's own. Reads that answer from the caches (a highway
type, a tier-0 scalar) are asserted against whether the database records them, which the check does not decide. The
witnesses are forgotten before and after.
"""
import json
import secrets
import sys

import _lpm

check = _lpm.check
DOG, WARM, CAT, COLD = ('zq' + word for word in ('vorlin', 'tamber', 'kesh', 'filum'))


def facts_recipe(name):
    return (f'name {name}\nmatch *.tsv\ntier row by \\n\nnames thing value\ntier field by \\t\nthing row thing\n'
            'content thing value\nrelation "has value" value\nattest row value\n')


def predecessor(hex_id):
    """The ID just before one, for the library's exclusive `after` cursor."""
    return format(int(hex_id, 16) - 1, '032x')


def main():
    with check.Fixture('read-snapshot') as fixture:
        sources = {}
        for part in ('first', 'second'):
            name, root = _lpm.source(fixture, part, lines='class AcademicCurated', recipes={'facts.recipe': facts_recipe(f'check-read-snapshot-{part}')})
            (root / 'facts.tsv').write_text(f'{DOG}\t{WARM}\n{CAT}\t{COLD}\n', encoding='utf-8', newline='\n')
            sources[part] = name
            if part == 'first':
                # Different packaging ASTs reference the same text entities, parsed by the normal grammar pipeline.
                directory = fixture.recipes / name
                (directory / 'xml.recipe').write_text('name check-read-snapshot-xml\nmatch *.xml\ngrammar xml\n', encoding='utf-8', newline='\n')
                (directory / 'json.recipe').write_text('name check-read-snapshot-json\nmatch *.json\ngrammar json\n', encoding='utf-8', newline='\n')
                fixture.guard(name)
                (root / 'facts.xml').write_text(f'<facts><row value="{DOG}">{DOG}</row></facts>', encoding='utf-8')
                (root / 'facts.json').write_text(json.dumps({'thing': DOG, 'nested': [DOG]}, separators=(',', ':')), encoding='utf-8')
        fixture.forget()
        _lpm.ingest(fixture, sources['first'])
        first_trunks = set(fixture.trunks(sources['first']))
        if len(first_trunks) != 1:
            raise AssertionError(f'{sources["first"]}: witness trunks {first_trunks}')
        application = 'lpm-check-read-snapshot-' + secrets.token_hex(6)

        def connections():
            rows = check.sql('SELECT pid,(xact_start IS NULL AND backend_xmin IS NULL)::int FROM pg_stat_activity'
                             " WHERE application_name=:'application' ORDER BY pid", variables={'application': application})
            result = {int(pid): int(idle) for pid, idle in (line.split('|') for line in rows.splitlines() if line)}
            assert len(result) == 1 and all(result.values()), result
            return set(result)

        with _lpm.Server(fixture, application) as server:
            request = server.request
            first = request('/v1/search', {'input': DOG})
            before = {row['trunk'] for row in first['attestations'] if row['witness']}
            assert before == first_trunks, (before, first_trunks, first)
            counts = {row['entity']: row['matches'] for row in first['claims'] if row['stood']}
            assert counts and all(value > 0 for value in counts.values()), first
            pids = connections()
            # The ID entry point discovers tiers and both constituent layers from stored Native rows, without
            # depending on text decomposition or leaf names. The source trunk exercises nested, mixed-tier content.
            trunk = next(iter(before))
            direct = request('/v1/search', {'id': first['id']})
            assert direct['physicality'] == first['physicality'] and direct['claims'] == first['claims'], direct
            source = request('/v1/search', {'id': trunk})
            assert source['physicality'] and source['constituents'], source
            assert any(part.get('constituents') for part in source['constituents']), source
            assert all(part['tier'] >= 0 for part in source['constituents']), source
            library = request('/v1/library', {'after': predecessor(trunk)})
            schema = request('/v1/schema')
            assert schema['schema'] == 'laplace-view-schema/v1' and len(schema['highway']) == 64, schema
            assert any(bank['name'] == 'kind' and bank['encoding'] == 'flags' for bank in schema['banks']), schema
            for bank in schema['banks']:
                assert len(bank['values']) == bank['slots'], bank
                assert [value['slot'] for value in bank['values']] == list(range(bank['slots'])), bank
                assert all(len(value['id']) == 32 and value['label'] for value in bank['values']), bank
            assert schema['views']['physicality']['name'] == 'Physicality', schema
            assert any(role['name'] == 'Metadata' for role in schema['vertex_roles']), schema
            assert first['stored_tier'] == first['computed_tier'] == 1, first
            assert direct['stored_tier'] == 1 and direct['computed_tier'] is None, direct
            assert first['storage'] == 'database' and 'entity_facets' in first, first
            assert first['discovery']['root_present'] and first['discovery']['matches'][0]['match'] == 'exact', first
            assert direct['discovery']['matches'] == request('/v1/search', {'id': first['id'].upper()})['discovery']['matches'], direct
            unknown = request('/v1/search', {'input': 'Qzxwvu' + secrets.token_hex(6) + 'Unobserved', 'display': True})
            assert not unknown['recorded'] and unknown['storage'] == 'computed', unknown
            assert not unknown['discovery']['root_present'], unknown
            assert unknown['id'] not in {match['id'] for match in unknown['discovery']['matches']}, unknown
            assert all(match['recorded'] or match['storage'] in ('highway-cache', 'tier0-cache')
                       for match in unknown['discovery']['matches']), unknown
            # No exact phrase was admitted. Its known words and their native paths remain independently
            # discoverable, including duplicate occurrences.
            phrase = request('/v1/search', {'input': f'{DOG} {CAT} {DOG}', 'display': True})
            assert not phrase['discovery']['root_present'], phrase
            members = [match for match in phrase['discovery']['matches'] if match['id'] == first['id']]
            assert len(members) == 2 and len({match['ordinal'] for match in members}) == 2, phrase
            assert all(match['recorded'] and match['match'] == 'member' and match['via'] == phrase['id'] for match in members), phrase
            rpc = request('/mcp', {'jsonrpc': '2.0', 'id': 71, 'method': 'tools/call',
                                   'params': {'name': 'search', 'arguments': {'input': f'{DOG} {CAT} {DOG}', 'display': True}}})
            assert json.loads(rpc['result']['content'][0]['text'])['discovery'] == phrase['discovery'], rpc
            bounded = request('/v1/search', {'input': 'Z' * 100})
            assert bounded['parts_total'] == 100 and not bounded['discovery']['members_complete'], bounded
            assert len(bounded['discovery']['matches']) == 64, bounded
            assert any(field['bank'] == 'kind' and any(value['label'] == 'file' for value in field['values'])
                       for field in source['physicality'][0]['facets']), source
            # A highway type and a tier-0 scalar read from the caches where the database does not record them.
            noun = request('/v1/embeddings', {'input': 'NOUN'})['data'][0]['id']
            typed = request('/v1/search', {'id': noun, 'display': True})
            assert typed['discovery']['root_present'], typed
            if typed['recorded']:
                assert typed['storage'] == 'database', typed
            else:
                assert typed['storage'] == 'highway-cache' and typed['discovery']['matches'][0]['storage'] == 'highway-cache', typed
                assert typed['canonical_tier'] == 1 and len(typed['parts']) == 4, typed
                assert any(label['id'] == noun and label['text'] == 'NOUN' for label in typed['labels']), typed
            scalar = request('/v1/embeddings', {'input': 'N'})['data'][0]['id']
            atom = request('/v1/search', {'id': scalar, 'display': True})
            assert atom['discovery']['root_present'], atom
            if atom['recorded']:
                assert atom['storage'] == 'database', atom
            else:
                assert atom['storage'] == 'tier0-cache' and atom['tier'] == 0, atom
                assert atom['discovery']['matches'][0]['storage'] == 'tier0-cache', atom
            discovered = {item['id']: item for item in library['items']}
            assert trunk in discovered, library
            assert discovered[trunk]['kind'] == 'source' and discovered[trunk]['children'], library
            graph = request('/v1/provenance', {'id': first['id']})
            assert graph['complete'] and graph['edges'], graph
            assert before <= {node['id'] for node in graph['nodes'] if node['metadata_trunk'] and node['root']}, graph
            assert all('position' in edge and 'ordinal' in edge for edge in graph['edges']), graph
            labels = [node['label'] for node in graph['nodes'] if node.get('trunk_kind') == 'file']
            assert all(any(label.endswith('facts.' + extension) for label in labels) for extension in ['tsv', 'xml', 'json']), graph
            assert first['physicality'][0]['n'] == 3 and first['physicality'][0]['tier'] == 1, first
            assert connections() == pids

            _lpm.ingest(fixture, sources['second'])
            second_trunks = set(fixture.trunks(sources['second']))
            second = request('/v1/search', {'input': DOG})
            after = {row['trunk'] for row in second['attestations'] if row['witness']}
            assert len(after) == 2 and before < after and after == first_trunks | second_trunks, second
            assert any(row['matches'] > counts[row['entity']] for row in second['claims'] if row['stood'] and row['entity'] in counts), second
            assert connections() == pids, 'a new read should reuse the same connection and plans'
    print('read-snapshot ok: XML, JSON and TSV reference the same tier-1 word and physicality; a source ingested between '
          'reads is seen by the next; one connection reused')
    return 0


if __name__ == '__main__':
    sys.exit(main())
