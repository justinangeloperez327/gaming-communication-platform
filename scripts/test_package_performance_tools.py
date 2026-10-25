"""Toolkit archives contain source tools only; no physical benchmark evidence."""
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest
import zipfile

_spec=importlib.util.spec_from_file_location('package_tools',Path(__file__).with_name('package-performance-tools.py'))
mod=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(mod)

class PackageTests(unittest.TestCase):
    def test_allowlist_hashes_reproducibility_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'source';source.mkdir()
            for name in mod.FILES:
                path=source/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('fixture tool source\n')
            (source/'private-secret.txt').write_text('must never be packaged')
            a,b=root/'a.zip',root/'b.zip'
            self.assertEqual(mod.package(source,a,'a'*40),mod.package(source,b,'a'*40))
            with zipfile.ZipFile(a) as archive:
                self.assertEqual(set(archive.namelist()),set(mod.FILES)|{'tool-manifest.json','README.md'})
                manifest=json.loads(archive.read('tool-manifest.json'))
                self.assertFalse(manifest['physical_evidence_included'])
                for item in manifest['files']:
                    self.assertEqual(hashlib.sha256(archive.read(item['path'])).hexdigest(),item['sha256'])
            with self.assertRaises((OSError,ValueError)):mod.package(source,a,'a'*40)

    def test_real_toolkit_runs_without_repository_imports(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);archive=root/'tools.zip'
            mod.package(Path(__file__).resolve().parent.parent,archive,'a'*40)
            extracted=root/'extracted';extracted.mkdir()
            with zipfile.ZipFile(archive) as bundle:bundle.extractall(extracted)
            environment=dict(os.environ);environment.pop('PYTHONPATH',None)
            command=[sys.executable,str(extracted/'scripts/qualify-desktop-performance.py'),'--init',str(root/'manifest.json'),'--scenario','voice']
            result=subprocess.run(command,cwd=root,env=environment,capture_output=True,text=True,check=False)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(json.loads((root/'manifest.json').read_text())['evidence_kind'],'operator_capture')

    def test_missing_sources_and_invalid_commit_fail(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            with self.assertRaises(ValueError):mod.package(root,root/'x.zip','wrong')
            with self.assertRaises(ValueError):mod.package(root,root/'x.zip','a'*40)
            self.assertFalse((root/'x.zip').exists())

if __name__=='__main__':unittest.main()
