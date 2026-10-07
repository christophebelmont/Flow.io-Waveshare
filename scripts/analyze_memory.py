#!/usr/bin/env python3
"""Summarize Flow.io WM traces; accepts serial logs and web boot-log replays."""
import argparse
import json
import re
from pathlib import Path

ANSI_ESCAPE = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')


def normalize_log(text):
    return ANSI_ESCAPE.sub('', text)


SAMPLE = re.compile(r'\bWM b=([0-9a-f]+) t=(\d+) id=(\d+) e=(\w+) '
                    r'i=(\d+) il=(\d+) im=(\d+) p=(\d+) pl=(\d+) a=(\d+)')
ROUTE = re.compile(r'\bWMR b=([0-9a-f]+) id=(\d+) method=(\S+) path=(\S*)')
STACK = re.compile(r'([^\s,]+) min=(\d+)/(\d+)B')
FIELDS = ('boot', 't', 'id', 'event', 'internal', 'largest_internal',
          'minimum_internal', 'psram', 'largest_psram', 'active')


def analyze(text):
    samples, routes, stacks = {}, {}, {}
    duplicates = 0
    for line in normalize_log(text).splitlines():
        match = SAMPLE.search(line)
        if match:
            values = match.groups()
            item = dict(zip(FIELDS, [v if n in (0, 3) else int(v)
                                     for n, v in enumerate(values)]))
            key = tuple(values)
            if key in samples:
                duplicates += 1
            samples[key] = item
        match = ROUTE.search(line)
        if match:
            boot, rid, method, path = match.groups()
            routes[(boot, int(rid))] = f'{method} {path}'
        for match in STACK.finditer(line):
            name, free, capacity = match.groups()
            free, capacity = int(free), int(capacity)
            previous = stacks.get(name)
            if previous is None or free < previous['minimum_free']:
                stacks[name] = dict(minimum_free=free, capacity=capacity,
                                    maximum_observed_use=capacity-free)
    boots = {}
    for item in samples.values():
        boots.setdefault(item['boot'], []).append(item)
    result = dict(duplicate_samples=duplicates, boots={}, stacks=stacks)
    for boot, items in boots.items():
        items.sort(key=lambda item: item["t"])
        requests, stages = {}, {}
        for item in items:
            if item['id']:
                requests.setdefault(item['id'], {})[item['event']] = item
            elif item['event'] not in ('periodic', 'capacity'):
                stages[item['event']] = item
        complete, incomplete = [], []
        for rid, phases in requests.items():
            row = dict(id=rid, route=routes.get((boot, rid), '?'), phases=list(phases))
            if 'enter' in phases and 'released' in phases:
                first, last = phases['enter'], phases['released']
                row.update(duration_ms=(last['t']-first['t']) & 0xffffffff,
                           internal_change=last['internal']-first['internal'],
                           psram_change=last['psram']-first['psram'])
                complete.append(row)
            else:
                incomplete.append(row)
        quiet = [s for s in items if s['event'] == 'periodic' and s['active'] == 0]
        result['boots'][boot] = dict(
            sample_count=len(items), minimum_internal=min(s['minimum_internal'] for s in items),
            minimum_observed_largest_internal=min(s['largest_internal'] for s in items),
            peak_tracked_requests=max(s['active'] for s in items),
            capacity_events=sum(s['event'] == 'capacity' for s in items),
            stages=stages, quiet_samples=quiet, completed=complete, incomplete=incomplete)
    return result


def stack_usage(directory):
    rows = []
    for path in Path(directory).rglob('*.su'):
        for line in path.read_text(errors='replace').splitlines():
            parts = line.rsplit('\t', 2)
            if len(parts) == 3 and parts[1].isdigit():
                rows.append(dict(function=parts[0], bytes=int(parts[1]), kind=parts[2]))
    return sorted(rows, key=lambda row: row['bytes'], reverse=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--stack-usage', type=Path, help='Build directory containing compiler .su files')
    args = parser.parse_args()
    result = analyze(args.log.read_text(errors='replace'))
    if args.stack_usage:
        result['compiler_frames'] = stack_usage(args.stack_usage)
    if args.json:
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return
    print(f"Échantillons rejoués éliminés : {result['duplicate_samples']}")
    if not result['boots']:
        print('Aucune trace WM complète. Flasher un build avec FLOW_MEMORY_DIAGNOSTICS=1.')
    for boot, run in result['boots'].items():
        print(f"\nSession {boot} : minimum interne {run['minimum_internal']} B ; "
              f"plus petit bloc maximal observé {run['minimum_observed_largest_internal']} B")
        print(f"Requêtes : pic suivi {run['peak_tracked_requests']}, "
              f"{len(run['completed'])} libérées, {len(run['incomplete'])} incomplètes, "
              f"{run['capacity_events']} dépassements de capacité")
        for name, sample in run['stages'].items():
            print(f"  {name}: interne={sample['internal']} PSRAM={sample['psram']} B")
        quiet = run['quiet_samples']
        if quiet:
            print(f"  Sans requête suivie (démarrage compris) : interne {quiet[0]['internal']} → {quiet[-1]['internal']} B "
                  f"({len(quiet)} points)")
        for row in sorted(run['completed'], key=lambda r: r['internal_change'])[:10]:
            print(f"  #{row['id']} {row['route']}: {row['duration_ms']} ms, "
                  f"variation globale interne {row['internal_change']:+} B")
    for name, row in result['stacks'].items():
        print(f"Pile {name}: minimum libre {row['minimum_free']}/{row['capacity']} B")
    for row in result.get('compiler_frames', [])[:20]:
        print(f"Trame compilateur {row['bytes']} B ({row['kind']}): {row['function']}")
    print('\nLes variations incluent les autres tâches ; une requête incomplète ne prouve pas une fuite. '
          'Les trames compilateur ne représentent pas la profondeur totale de pile.')


if __name__ == '__main__':
    main()
