"""One descriptor source for DataStore identity, formula references and UI configuration."""
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
RUNTIME_BASE = 32768

def load_catalog():
    entries = json.loads((ROOT / 'src/Core/Values/catalog.json').read_text())['values']
    equipment = json.loads((ROOT / 'src/Core/Values/equipment_metrics.json').read_text())
    for slot in range(equipment['slots']):
        for metric in equipment['metrics']:
            identity = equipment['base'] + slot * equipment['stride'] + metric['id']
            entries.append(dict(id=identity, reference=f"pd{slot:02}.{metric['key']}",
                label=metric['label'], group=f"Équipement {slot + 1}", unit=metric['unit'],
                kind='equipmentbinary' if metric['type']=='bool' else 'equipment', slot=slot,
                ha_default=False, ha_suffix=f"data_{identity}", topic=f"rt/value/{identity}"))
    for key in ('id', 'reference', 'ha_suffix'):
        if len({v[key] for v in entries}) != len(entries):
            raise ValueError('Duplicate data descriptor ' + key)
    limit = equipment['base'] + equipment['slots'] * equipment['stride']
    if len({m['id'] for m in equipment['metrics']}) != len(equipment['metrics']) or any(not 0 <= m['id'] < equipment['stride'] for m in equipment['metrics']):
        raise ValueError('Invalid equipment metric identities')
    if any(not 0 <= v['id'] < limit for v in entries):
        raise ValueError('Data ID outside registry')
    return entries

def write_header():
    equipment = json.loads((ROOT / 'src/Core/Values/equipment_metrics.json').read_text())
    symbols = ['#pragma once', '#include <stdint.h>', '// Generated stable metric identities; never renumber existing entries.', 'enum class EquipmentMetric : uint8_t {']
    for metric in equipment['metrics']:
        symbol = ''.join(part.capitalize() for part in metric['key'].split('_'))
        symbols.append(f"    {symbol} = {metric['id']},")
    symbols.append('};')
    for name, value in [('Base', equipment['base']), ('Stride', equipment['stride']), ('Slots', equipment['slots'])]:
        symbols.append(f'inline constexpr uint16_t EquipmentValue{name} = {value};')
    (ROOT/'src/Core/Generated/EquipmentMetric_Generated.h').write_text('\n'.join(symbols)+'\n')
    out = ['#pragma once', '// Generated from Core/Values/catalog.json; no runtime value storage.',
           '#include "Core/Values/Value.h"', '#include <string.h>',
           'enum class DataValueKind : uint8_t { Analog, Counter, Derived, Tank, Uptime, Digital, Equipment, EquipmentBinary };',
           'struct DataValueDescriptor { ValueId id; const char* reference; const char* label; const char* group; const char* unit; DataValueKind kind; uint8_t slot; bool defaultHa; const char* haSuffix; const char* topic; };',
           'inline constexpr DataValueDescriptor kDataValues[] = {']
    q = lambda s: json.dumps(s, ensure_ascii=True)
    for v in load_catalog():
        out.append('    {' + ', '.join([str(v['id']), q(v['reference']), q(v['label']), q(v['group']), q(v['unit']), 'DataValueKind::'+('EquipmentBinary' if v['kind']=='equipmentbinary' else v['kind'].capitalize()), str(v['slot']), str(v['ha_default']).lower(), q(v['ha_suffix']), q(v['topic'])]) + '},')
    out += ['};', 'inline constexpr size_t kDataValueCount = sizeof(kDataValues)/sizeof(kDataValues[0]);',
        'inline const DataValueDescriptor* dataValueDescriptor(ValueId id) { for (const auto& d : kDataValues) if (d.id == id) return &d; return nullptr; }',
        'inline bool dataValueCompatible(const DataValueDescriptor& d, const ValueMetadata& metadata) { return (d.kind == DataValueKind::Digital || d.kind == DataValueKind::EquipmentBinary) ? metadata.type == ValueType::Bool : !(d.kind == DataValueKind::Counter && d.id >= ValueIds::Digital && d.id < ValueIds::PulseRate) || metadata.type == ValueType::UInt64; }',
        'inline ValueId dataValueReference(const char* text, size_t length) { for (const auto& d : kDataValues) if (strlen(d.reference) == length && memcmp(d.reference, text, length) == 0) return d.id; return VALUE_INVALID; }']
    out += ['inline bool dataValueIsEquipment(const DataValueDescriptor& d) { return d.kind == DataValueKind::Equipment || d.kind == DataValueKind::EquipmentBinary; }',
        'inline bool dataValueIsBinary(const DataValueDescriptor& d) { return d.kind == DataValueKind::Digital || d.kind == DataValueKind::EquipmentBinary; }']
    (ROOT/'src/Core/Generated/DataValueCatalog_Generated.h').write_text('\n'.join(out)+'\n')

def runtime_entries():
    return [dict(id=RUNTIME_BASE+v['id'], runtimeId=RUNTIME_BASE+v['id'], dataValueId=v['id'], module='datastore', moduleId=0,
        valueId=v['id'], key=v['reference'], label=v['label'], type='bool' if v['kind'] in ('digital', 'equipmentbinary') else 'double', domain='sondes', group=v['group'], unit=v['unit'],
        decimals=1, order=v['id'], display='value', selectableOnly=True, formulaReference=v['reference']) for v in load_catalog()]

def extend_config(docs, modules, meta):
    modules['data'] = {'label':'Données', 'hidden': True}
    modules['data/value'] = {'label':'Exposition et historique'}
    options=meta.setdefault('enum_sets',{}).setdefault('tft_s3_dashboard_runtime_ui',[])
    existing={o.get('value') for o in options}
    for v in load_catalog():
        path=f"data/value/d{v['id']:03}"
        modules[path]={'label':v['label']}
        docs[path+'/publish_ha']={'type':'Bool','label':'Publier dans Home Assistant','help':'Choix appliqué après redémarrage. Indépendant des champs TFT/Web et des formules.'}
        docs[path+'/usable']={'type':'Bool','label':'Utilisable dans GUI, Web, TFT et valeurs calculées','help':'Choix commun aux affichages et aux sources de formules, appliqué après redémarrage.'}
        docs[path+'/history']={'type':'Bool','label':'Historiser','help':'Choix appliqué après redémarrage.'}
        if RUNTIME_BASE+v['id'] not in existing:
            options.append({'value':RUNTIME_BASE+v['id'],'label':v['label'],'data_value_id':v['id']})
