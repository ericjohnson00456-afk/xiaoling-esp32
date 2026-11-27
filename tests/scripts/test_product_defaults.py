import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('product_defaults', ROOT / 'scripts/generate_product_defaults.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ProductDefaultsTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.board = self.root / 'board.json'
        self.output = self.root / 'product_defaults.generated.h'
        (self.root / 'defaults.json').write_text(json.dumps(
            {'product_id': 'global-pid', 'secret_id': 'global-sid'}))

    def generate(self, manufacturer):
        self.board.write_text(json.dumps({'manufacturer': manufacturer}))
        module.generate(self.board, self.root, self.output)
        return self.output.read_text()

    def test_new_manufacturer_is_data_only_and_missing_fields_fall_through(self):
        (self.root / 'new-vendor').mkdir()
        (self.root / 'new-vendor/defaults.json').write_text(json.dumps({'product_id': 'vendor-pid'}))
        self.generate('new-vendor')
        source = self.root / 'check.cc'
        source.write_text('''#include "product_defaults.generated.h"
#include "system/product_identity.h"
#include <cassert>
int main() {
    using namespace ProductDefaults;
    assert(ResolveIdentityField("", "", kManufacturerProductId, kGlobalProductId) == "vendor-pid");
    assert(ResolveIdentityField("", "", kManufacturerSecretId, kGlobalSecretId) == "global-sid");
}
''')
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'main'), str(source), '-o', str(self.root / 'check')], check=True)
        subprocess.run([str(self.root / 'check')], check=True)

    def test_switching_to_manufacturer_without_defaults_clears_previous_values(self):
        (self.root / 'vendor').mkdir()
        (self.root / 'vendor/defaults.json').write_text('{"secret_id":"vendor-sid"}')
        self.generate('vendor')
        for manufacturer in ('other-vendor', ''):
            text = self.generate(manufacturer)
            self.assertIn('kManufacturerProductId = "";', text)
            self.assertIn('kManufacturerSecretId = "";', text)
            self.assertNotIn('vendor-sid', text)
            self.assertIn('kGlobalSecretId = "global-sid";', text)
