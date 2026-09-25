"""Static regression checks for the deployable YAML (requires PyYAML)."""
import json
import re
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
DASHBOARD = ROOT / 'docs/integration/home_assistant_dashboard_flowio.yaml'


class UniqueKeyLoader(yaml.SafeLoader):
    def construct_mapping(self, node, deep=False):
        keys = [self.construct_object(key, deep=deep) for key, _ in node.value]
        if len(keys) != len(set(keys)):
            raise ValueError(f'Duplicate YAML key at line {node.start_mark.line + 1}')
        return super().construct_mapping(node, deep=deep)


def mappings(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from mappings(child)
    elif isinstance(value, list):
        for child in value:
            yield from mappings(child)


class DashboardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = DASHBOARD.read_text()
        cls.config = yaml.load(cls.text, Loader=UniqueKeyLoader)
        cls.nodes = list(mappings(cls.config))

    def test_native_views_and_sections(self):
        expected = ['piscine', 'filtration', 'ph', 'desinfection',
                    'oxygene', 'equipements', 'alarmes', 'systeme']
        self.assertEqual([v['path'] for v in self.config['views']], expected)
        for view in self.config['views']:
            self.assertEqual(view['type'], 'sections')
            self.assertNotIn('cards', view)
            for section in view['sections']:
                self.assertEqual(section['type'], 'grid')
                self.assertTrue(section['cards'])
                self.assertNotIn('columns', section)
                self.assertNotIn('square', section)
                for card in section['cards']:
                    self.assertIn('type', card)

    def test_only_existing_hacs_components(self):
        allowed = {'custom:mushroom-legacy-template-card',
                   'custom:mushroom-entity-card', 'custom:mushroom-chips-card',
                   'custom:mini-graph-card', 'custom:modern-circular-gauge'}
        custom = {n['type'] for n in self.nodes
                  if str(n.get('type', '')).startswith('custom:')}
        self.assertEqual(custom, allowed)
        for token in ('[[[', '${', '<script', 'javascript:', 'custom:flowio-pool-card'):
            self.assertNotIn(token, self.text)
        self.assertNotIn('resources', self.config)
        self.assertFalse((ROOT / 'docs/integration/home-assistant/flowio-pool-card.js').exists())

    def test_entity_coverage_and_actions(self):
        ids = set(re.findall(r'\b(?:sensor|binary_sensor|switch|number|select|button)\.fio_[a-z0-9_]+', json.dumps(self.config)))
        self.assertEqual(len(ids), 115)
        paths = {v['path'] for v in self.config['views']}
        for node in self.nodes:
            if node.get('action') == 'navigate':
                self.assertIn(node['navigation_path'], paths)
            if node.get('action') == 'perform-action':
                self.assertEqual(node['perform_action'], 'button.press')
                self.assertTrue(node['target']['entity_id'].startswith('button.fio_'))
            if node.get('type') == 'simple-entity':
                self.assertEqual(node['tap_action'], {'action': 'more-info'})
        reset_rows = [n for n in self.nodes if n.get('type') == 'conditional']
        self.assertEqual(len(reset_rows), 4)
        for node in reset_rows:
            alarm, button = node['conditions']
            self.assertEqual(alarm['state'], 'on')
            self.assertEqual(button['state_not'], 'unavailable')
            self.assertEqual(button['entity'], node['row']['entity'])

    def test_gauges_and_history_domains(self):
        gauges = [n for n in self.nodes if n.get('type') == 'custom:modern-circular-gauge']
        self.assertEqual(len(gauges), 4)
        for gauge in gauges:
            self.assertTrue(gauge['needle'])
            self.assertEqual(gauge['gauge_type'], 'standard')
            self.assertEqual(len(gauge['segments']), 5)
            self.assertIn('is_number', gauge['min'])
            self.assertIn('is_number', gauge['max'])
            for segment in gauge['segments']:
                self.assertIn('#C7C7CC', segment['color'])
        for node in self.nodes:
            if node.get('type') == 'custom:mini-graph-card':
                for entity in node['entities']:
                    self.assertTrue(entity['entity'].startswith(('sensor.', 'binary_sensor.')))
        chemistry = next(n for n in self.nodes if n.get('name') == 'Chimie')
        self.assertEqual(chemistry['entities'][1]['y_axis'], 'secondary')

    def test_optional_theme(self):
        theme = yaml.load((ROOT / 'docs/integration/home-assistant/flowio-theme.yaml').read_text(), Loader=UniqueKeyLoader)
        self.assertIn('Flowio Clair', theme)
        self.assertEqual(theme['Flowio Clair']['ha-card-background'], '#FFFFFF')


if __name__ == '__main__':
    unittest.main(verbosity=2)
