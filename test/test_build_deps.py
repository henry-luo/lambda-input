#!/usr/bin/env python3
"""Check imported build contracts, native probes, and a real dependency consumer."""

import copy
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from build_config import load_build_config, resolve_source_sets
from dependency_builder import (CompilerProbe, expand, generate_premake, projects_for,
                                render_template, reset_build_inputs)


class DependencyBuildTest(unittest.TestCase):
    def setUp(self):
        (ROOT / 'temp').mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / 'temp')
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)

    def test_imports_preserve_sources_and_reject_cycles_and_duplicates(self):
        leaf = self.work / 'leaf.json'
        main = self.work / 'main.json'
        leaf.write_text(json.dumps({'targets': [{'name': 'provider'}], 'headers': {'A': 1},
                                    'description': 'dependency 源'}, ensure_ascii=False), encoding='utf-8')
        main.write_text(json.dumps({'config_imports': ['leaf.json'], 'targets': [{'name': 'host'}],
                                    'headers': {'B': 2}}))
        resolved = load_build_config(main)
        self.assertEqual([target['name'] for target in resolved['targets']], ['provider', 'host'])
        self.assertEqual(resolved['headers'], {'A': 1, 'B': 2})
        self.assertEqual(resolved['description'], 'dependency 源')
        main.write_text(json.dumps({'config_imports': ['leaf.json'], 'targets': [{'name': 'provider'}]}))
        with self.assertRaisesRegex(ValueError, 'duplicate targets'):
            load_build_config(main)
        leaf.write_text(json.dumps({'config_imports': ['main.json']}))
        with self.assertRaisesRegex(ValueError, 'cyclic'):
            load_build_config(main)

    def test_import_can_select_owned_interfaces(self):
        (self.work / 'owner.json').write_text(json.dumps({
            'targets': [{'name': 'provider'}], 'private_build_recipe': {'sources': ['vendor.c']}}))
        main = self.work / 'main.json'
        main.write_text(json.dumps({'config_imports': [{'path': 'owner.json',
                          'root_env': 'UNSET_SETUP_TEST_ROOT', 'root_path': 'owner.json',
                          'include_keys': ['targets']}]}))
        resolved = load_build_config(main)
        self.assertEqual([target['name'] for target in resolved['targets']], ['provider'])
        self.assertNotIn('private_build_recipe', resolved)
        main.write_text(json.dumps({'config_imports': [{'path': 'owner.json',
                          'root_env': 'UNSET_SETUP_TEST_ROOT', 'root_path': 'owner.json',
                          'include_keys': ['missing']}]}))
        with self.assertRaisesRegex(ValueError, 'missing config import keys'):
            load_build_config(main)

    def test_import_can_select_a_sibling_root(self):
        default = self.work / 'default'
        selected = self.work / 'selected'
        default.mkdir()
        selected.mkdir()
        (default / 'targets.json').write_text(json.dumps({'targets': [{'name': 'default'}]}))
        (selected / 'targets.json').write_text(json.dumps({'targets': [{'name': 'selected'}]}))
        main = self.work / 'main.json'
        main.write_text(json.dumps({'config_imports': [{'path': 'default/targets.json',
            'root_env': 'LAMBDA_TEST_IMPORT_ROOT', 'root_path': 'targets.json'}]}))
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop('LAMBDA_TEST_IMPORT_ROOT', None)
            self.assertEqual(load_build_config(main)['targets'][0]['name'], 'default')
            os.environ['LAMBDA_TEST_IMPORT_ROOT'] = 'selected'
            self.assertEqual(load_build_config(main)['targets'][0]['name'], 'selected')

    def test_templates_disable_optional_features_and_require_metadata(self):
        template = '#cmakedefine HAVE_MISSING @MISSING@\n#cmakedefine01 HAVE_PRESENT\n#define VERSION "@VERSION@"\n'
        self.assertEqual(render_template(template, {'HAVE_PRESENT': 1, 'VERSION': '1.2'}),
                         '/* #undef HAVE_MISSING */\n#define HAVE_PRESENT 1\n#define VERSION "1.2"\n')
        with self.assertRaisesRegex(ValueError, 'missing template variable'):
            render_template('#define VERSION @VERSION@', {})

    def test_platform_selection_is_shared_and_idempotent(self):
        target = {'defines': {'BASE': 1}, 'source_sets': [
            {'platforms': ['windows'], 'defines': {'WINDOWS': 1}},
            {'architectures': ['arm64'], 'defines': {'ARM': 1}}]}
        resolve_source_sets(target, 'linux', 'arm64')
        resolve_source_sets(target, 'linux', 'arm64')
        self.assertEqual(target, {'defines': {'BASE': 1, 'ARM': 1}})

    def test_compiler_probes_distinguish_missing_features(self):
        compiler = os.environ.get('CC') or shutil.which('cc')
        probe = CompilerProbe(compiler, self.work, dict(os.environ, CPPFLAGS='-DLAMBDA_PROBE_ENV=1', CFLAGS='-Werror'))
        values = probe.values({'headers': {'HAVE_STDDEF_H': 'stddef.h', 'HAVE_MISSING': 'lambda_absent_header.h'},
                               'expressions': {'HAVE_ENV_FLAGS': 'LAMBDA_PROBE_ENV'},
                               'types': {'COUNT_TYPE': ['lambda_absent_type', 'size_t']},
                               'sizes': {'SIZEOF_CHAR': 'char'}})
        self.assertNotIn('HAVE_MISSING', values)
        self.assertEqual(values['HAVE_ENV_FLAGS'], 1)
        self.assertEqual(values['COUNT_TYPE'], 'size_t')
        self.assertEqual(values['SIZEOF_CHAR'], 1)

    def test_source_validation_and_native_archive_consumer(self):
        compiler = os.environ.get('CC') or shutil.which('cc')
        system = {'Darwin': 'macos', 'Linux': 'linux'}.get(platform.system(), 'windows')
        variables = {'source': str(self.work), 'build': str(self.work / 'build'),
                     'system': system, 'architecture': platform.machine()}
        (self.work / 'src').mkdir()
        (self.work / 'config.h').write_text('#define DEPENDENCY_VALUE 42\n')
        (self.work / 'src/probe.cc').write_text('#include DEPENDENCY_HEADER\n'
            '#ifndef DEPENDENCY_FILE_FLAG\n#error missing file-specific definition\n#endif\n'
            'extern "C" int dependency_answer(void) { return DEPENDENCY_VALUE; }\n')
        (self.work / 'app.c').write_text('int dependency_answer(void); int main(void) { return dependency_answer() != 42; }\n')
        recipe = expand({'projects': [{'name': 'provider', 'lang': 'c++17',
                                        'source_patterns': ['${source}/src/*.cc'],
                                        'includes': ['${source}'],
                                        'defines': ['DEPENDENCY_HEADER=\\"config.h\\"'],
                                        'file_rules': [{'files': ['${source}/src/**.cc'],
                                                        'defines': ['DEPENDENCY_FILE_FLAG=1']}]}]}, variables)
        projects = projects_for(recipe, variables)
        script = self.work / 'premake5.lua'
        generate_premake(recipe, variables, projects, compiler, script)
        premake = os.environ.get('PREMAKE5_BIN') or shutil.which('premake5')
        env = dict(os.environ, TMPDIR=str(self.work))
        commands = [[premake, '--file=' + str(script), 'gmake'],
                    ['make', '-C', str(self.work / 'build/make'), 'config=release_native'],
                    [*shlex.split(compiler), str(self.work / 'app.c'), str(self.work / 'build/lib/libprovider.a'),
                     '-o', str(self.work / 'consumer.exe')], [str(self.work / 'consumer.exe')]]
        for command in commands:
            result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
        recipe['projects'][0]['source_patterns'] = [str(self.work / '*.missing')]
        with self.assertRaisesRegex(ValueError, 'no dependency sources'):
            projects_for(recipe, variables)

    def test_make_selection_keeps_static_only_sources_without_building(self):
        (self.work / 'Makefile').write_text('WIN32RES = version.o\nOBJS = base.o $(WIN32RES)\nOBJS_STATIC = auth.o\nall:\n\t@false\n')
        for name in ('base.c', 'auth.c'):
            (self.work / name).write_text('/* source-selection fixture */\n')
        recipe = {'projects': [{'name': 'client', 'make_sources': {
            'directory': str(self.work), 'source_directory': str(self.work),
            'variables': ['OBJS', 'OBJS_STATIC'], 'make_variables': {'WIN32RES': ''}}}]}
        variables = {'build': str(self.work), 'system': 'macos', 'architecture': 'arm64'}
        projects = projects_for(recipe, variables)
        self.assertEqual([Path(path).name for path in projects[0]['source_files']], ['base.c', 'auth.c'])

    def test_upstream_source_manifest_is_read_without_modifying_source(self):
        source = self.work / 'source'
        build = self.work / 'build'
        source.mkdir()
        build.mkdir()
        (source / 'Makefile.inc').write_text('CSOURCES = first.c second.c\n')
        for name in ('first.c', 'second.c'):
            (source / name).write_text('/* source-selection fixture */\n')
        recipe = {'projects': [{'name': 'provider', 'make_sources': {
            'directory': str(source), 'makefile': 'Makefile.inc',
            'source_directory': str(source), 'variables': ['CSOURCES']}}]}
        variables = {'build': str(build), 'system': 'macos', 'architecture': 'arm64'}
        projects = projects_for(recipe, variables)
        self.assertEqual([Path(path).name for path in projects[0]['source_files']], ['first.c', 'second.c'])
        self.assertFalse((source / 'lambda-variable.mk').exists())
        self.assertTrue((build / 'lambda-variable.mk').is_file())

    def test_changed_build_identity_discards_only_owned_outputs(self):
        stamp = self.work / 'identity.json'
        objects = self.work / 'objects'
        archive = self.work / 'libprovider.a'
        unrelated = self.work / 'keep.txt'
        unrelated.write_text('keep')
        self.assertTrue(reset_build_inputs(stamp, 'first', [objects, archive]))
        objects.mkdir()
        (objects / 'object.o').write_text('old')
        archive.write_text('old')
        self.assertFalse(reset_build_inputs(stamp, 'first', [objects, archive]))
        self.assertTrue(archive.exists())
        self.assertTrue(reset_build_inputs(stamp, 'second', [objects, archive]))
        self.assertFalse(objects.exists())
        self.assertFalse(archive.exists())
        self.assertEqual(unrelated.read_text(), 'keep')

    def test_thorvg_keeps_png_and_jpeg_loaders_on_each_platform(self):
        config = load_build_config(ROOT / 'build_vendor_deps_config.json')
        recipe = config['dependency_builds']['thorvg']
        for system in ('macos', 'linux', 'windows'):
            with self.subTest(system=system):
                project = copy.deepcopy(recipe['projects'][0])
                header = copy.deepcopy(recipe['generated_files'][0])
                resolve_source_sets(project, system, 'x86_64')
                resolve_source_sets(header, system, 'x86_64')
                self.assertTrue(any('external_png/' in source for source in project['source_files']))
                self.assertTrue(any('external_jpg/' in source for source in project['source_files']))
                self.assertEqual(header['defines']['THORVG_PNG_LOADER_SUPPORT'], 1)
                self.assertEqual(header['defines']['THORVG_JPG_LOADER_SUPPORT'], 1)


if __name__ == '__main__':
    unittest.main()
