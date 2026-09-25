"""Offline integration tests: real temporary Git repos, mocked GitHub/model/build."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PipelineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.work = self.root / 'work'
        self.work.mkdir()
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.api = self.root / 'api.json'
        self.api.write_text(json.dumps({'issues': [], 'comments': [], 'releases': [], 'closed': [], 'notes': []}))
        self.env = dict(os.environ, WORKSPACE=str(self.work), GITHUB_OWNER='owner',
                        GITHUB_REPO='repo', GITHUB_TOKEN='test-only', GITHUB_BRANCH='master',
                        TEST_API=str(self.api), PATH=f'{self.bin}:{os.environ["PATH"]}')
        self.write(self.bin / 'gh', '''#!/usr/bin/env python3
import json, os, re, sys
from pathlib import Path
p=Path(os.environ['TEST_API']); d=json.loads(p.read_text()); a=sys.argv[1:]
def save(): p.write_text(json.dumps(d))
def post_fields(a):
    f={}
    i=1
    while i < len(a):
        t=a[i]
        if t in ('-f','-F') and i+1 < len(a) and '=' in a[i+1]:
            k,v=a[i+1].split('=',1)
            if t=='-F':
                try: v=json.loads(v)
                except Exception: pass
            f[k]=v; i+=2
        elif t in ('-f','-F') and i+2 < len(a):
            k=a[i+1]; v=a[i+2]
            if t=='-F':
                try: v=json.loads(v)
                except Exception: pass
            f[k]=v; i+=3
        else:
            i+=1
    return f
def endpoint(a):
    return next((x for x in a[1:] if x.startswith('repos/')), None)
if a[0]=='api':
    url=endpoint(a)
    m=re.search(r'/releases/(\\d+)$', url)
    if '--method' in a and 'POST' in a and url.endswith('/releases'):
        f=post_fields(a)
        rel={'id': d.get('next_id', 1000), 'tag_name': f.get('tag_name'), 'target_commitish': f.get('target_commitish'), 'draft': bool(f.get('draft')), 'prerelease': False, 'assets': []}
        d['next_id']=rel['id']+1; d['post_created']=True; d['releases'].insert(0, rel); save(); print(json.dumps(rel))
    elif m:
        rid=int(m.group(1)); rel=next(x for x in d['releases'] if x.get('id')==rid)
        print(json.dumps(rel))
    elif '/issues?' in url: print(json.dumps(d['issues']))
    elif '/releases/tags/' in url: print(json.dumps(d['releases'][0]))
    elif 'releases?per_page' in url and d.get('post_created') and os.environ.get('FAIL_LIST_LAG'):
        print('[]')
    else: print(json.dumps(d['releases']))
elif a[:2]==['issue','view']:
    if 'state,comments' in a: print(json.dumps({'state':'OPEN','comments':d['comments']}))
    else: print('CLOSED' if a[2] in d['closed'] else 'OPEN')
elif a[:2]==['issue','close']: d['closed'].append(a[2]); save()
elif a[:2]==['issue','comment']: d['notes'].append(a[2]); save()
elif a[:2]==['release','upload']:
    if os.environ.get('FAIL_UPLOAD'): sys.exit(4)
    d['releases'][0]['assets']=[{'name':'MinimalKanban.exe','size':8}]; save()
elif a[:2]==['release','edit']: d['releases'][0]['draft']=False; save()
else: raise SystemExit('Unexpected gh call: '+repr(a))
''')
        shutil.copytree(ROOT / 'server', self.work / 'server', ignore=shutil.ignore_patterns('__pycache__'))
        (self.work / '.bugbot').mkdir()
        self.write(self.work / '.gitignore', '.bugbot/server-state/\n.bugbot/lock\nreports/\nMinimalKanban.exe\n')
        self.write(self.work / 'minimal_kanban.cpp', 'static const wchar_t* APP_VERSION = L"0.1.0";\n')
        self.write(self.work / 'build.sh', '#!/bin/bash\n[[ -z "${FAIL_BUILD:-}" ]] || exit 3\nprintf binary > MinimalKanban.exe\n')
        self.write(self.work / '.bugbot/run_bugbot.sh', '''#!/bin/bash
[[ -z "${FAIL_AGENT:-}" ]] || exit 2
printf '// fix\n' >> minimal_kanban.cpp
git add minimal_kanban.cpp
git commit -qm fix
mkdir -p reports/done
mv reports/queue/*.md reports/done/
''')
        self.git('init', '-b', 'master')
        self.git('config', 'user.name', 'Test')
        self.git('config', 'user.email', 'test@example.invalid')
        self.git('add', '.')
        self.git('commit', '-qm', 'initial')
        self.remote = self.root / 'remote.git'
        subprocess.run(['git', 'init', '--bare', str(self.remote)], check=True, capture_output=True)
        self.git('remote', 'add', 'origin', str(self.remote))
        self.git('push', '-u', 'origin', 'master')

    def write(self, path, text):
        path.write_text(text)
        path.chmod(0o755)

    def git(self, *args):
        return subprocess.run(['git', *args], cwd=self.work, check=True, capture_output=True, text=True).stdout.strip()

    def data(self, **updates):
        d = json.loads(self.api.read_text())
        d.update(updates)
        self.api.write_text(json.dumps(d))
        return d

    def issue(self, n):
        return dict(number=n, title=f'Bug {n}', body='Repro', created_at='2026-01-01T00:00:00Z',
                    user={'login': 'owner'})

    def run_script(self, script='run-daily.sh', **env):
        return subprocess.run(['bash', str(self.work / 'server' / script)], cwd=self.work,
                              env=dict(self.env, **env), capture_output=True, text=True)

    def test_success_closes_only_current_report(self):
        old = self.work / 'reports/done'
        old.mkdir(parents=True)
        (old / '20250101-#10-old.md').write_text('old report')
        self.data(issues=[self.issue(1)])
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.data()['closed'], ['1'])
        self.assertFalse(self.data()['releases'][0]['draft'])
        self.assertEqual(self.git('rev-parse', 'HEAD'), self.git('rev-parse', 'origin/master'))

    def test_build_failure_retries_same_version(self):
        self.data(issues=[self.issue(1)])
        self.assertNotEqual(self.run_script(FAIL_BUILD='1').returncode, 0)
        self.assertEqual(self.data()['closed'], [])
        self.assertEqual(self.data()['releases'], [])
        sha = self.git('rev-parse', 'HEAD')
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.git('rev-parse', 'HEAD'), sha)
        self.assertEqual(self.data()['closed'], ['1'])

    def test_upload_failure_keeps_draft_and_ticket_open(self):
        self.data(issues=[self.issue(1)])
        self.assertNotEqual(self.run_script(FAIL_UPLOAD='1').returncode, 0)
        self.assertEqual(self.data()['closed'], [])
        self.assertTrue(self.data()['releases'][0]['draft'])
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.data()['closed'], ['1'])

    def test_draft_publish_succeeds_despite_list_endpoint_lag(self):
        # Regression: the releases list endpoint can lag a freshly created draft
        # (and a draft has no git tag), so verification must use point reads by id.
        self.data(issues=[self.issue(1)])
        r = self.run_script(FAIL_LIST_LAG='1')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(self.data()['releases'][0]['draft'])
        self.assertEqual(self.data()['closed'], ['1'])

    def test_resume_does_not_regress_advanced_remote(self):
        self.data(issues=[self.issue(1)])
        self.assertNotEqual(self.run_script(FAIL_UPLOAD='1').returncode, 0)
        remote_head = self.git('rev-parse', 'origin/master')
        other = self.root / 'other'
        subprocess.run(['git', 'clone', str(self.remote), str(other)], check=True, capture_output=True)
        subprocess.run(['git', 'config', 'user.name', 'T'], cwd=other, check=True, capture_output=True)
        subprocess.run(['git', 'config', 'user.email', 't@e.i'], cwd=other, check=True, capture_output=True)
        (other / 'deploy.txt').write_text('hand pushed\n')
        subprocess.run(['git', 'add', '.'], cwd=other, check=True, capture_output=True)
        subprocess.run(['git', 'commit', '-qm', 'deploy commit'], cwd=other, check=True, capture_output=True)
        subprocess.run(['git', 'push'], cwd=other, check=True, capture_output=True)
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.data()['closed'], ['1'])
        self.assertNotEqual(self.git('rev-parse', 'origin/master'), remote_head)

    def test_push_failure_does_not_release(self):
        self.write(self.remote / 'hooks/pre-receive', '#!/bin/sh\nexit 1\n')
        self.data(issues=[self.issue(1)])
        self.assertNotEqual(self.run_script().returncode, 0)
        self.assertEqual(self.data()['releases'], [])
        self.assertEqual(self.data()['closed'], [])

    def test_agent_failure_requires_inspection(self):
        self.data(issues=[self.issue(1)])
        self.assertNotEqual(self.run_script(FAIL_AGENT='1').returncode, 0)
        self.assertIn('Interrupted agent', self.run_script().stderr)
        self.assertEqual(self.data()['closed'], [])

    def test_pull_failure_does_not_run_agent(self):
        self.git('remote', 'set-url', 'origin', str(self.root / 'missing.git'))
        self.data(issues=[self.issue(1)])
        before = self.git('rev-parse', 'HEAD')
        self.assertNotEqual(self.run_script().returncode, 0)
        self.assertEqual(self.git('rev-parse', 'HEAD'), before)
        self.assertEqual(self.data()['closed'], [])

    def test_real_agent_runner_propagates_failure(self):
        shutil.copy2(ROOT / '.bugbot/run_bugbot.sh', self.work / '.bugbot/run_bugbot.sh')
        (self.work / '.bugbot/config.json').write_text(json.dumps({'model': 'test/model', 'opencodePath': 'opencode'}))
        (self.work / '.bugbot/BUGBOT.md').write_text('Test prompt')
        queue = self.work / 'reports/queue'
        queue.mkdir(parents=True)
        (queue / '20260101-test.md').write_text('Test report')
        self.write(self.bin / 'opencode', '#!/bin/sh\nexit 17\n')
        result = subprocess.run(['bash', '.bugbot/run_bugbot.sh'], cwd=self.work,
                                env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 17, result.stdout + result.stderr)

    def test_blocked_only_fresh_owner_reply(self):
        blocked = self.work / 'reports/blocked'
        blocked.mkdir(parents=True)
        report = blocked / '20260101-#1-bug.md'
        report.write_text('## Questions for owner\nWhich OS?')
        os.utime(report, (1767225600, 1767225600))
        self.data(comments=[{'author': {'login': 'owner'}, 'createdAt': '2025-01-01T00:00:00Z', 'body': 'old'},
                            {'author': {'login': 'other'}, 'createdAt': '2026-02-01T00:00:00Z', 'body': 'other'}])
        self.assertEqual(self.run_script('sync_issues.sh').returncode, 0)
        self.assertTrue(report.exists())
        self.assertEqual(self.data()['notes'], ['1'])
        self.data(comments=[{'author': {'login': 'owner'}, 'createdAt': '2026-02-01T00:00:00Z', 'body': 'Windows 11'}])
        self.assertEqual(self.run_script('sync_issues.sh').returncode, 0)
        self.assertFalse(report.exists())
        self.assertIn('Windows 11', (self.work / 'reports/queue' / report.name).read_text())

    def test_issue_numbers_are_exact_and_prs_excluded(self):
        done = self.work / 'reports/done'
        done.mkdir(parents=True)
        (done / '20260101-#10-bug.md').write_text('mentions #1')
        self.data(issues=[self.issue(1), self.issue(10), dict(self.issue(2), pull_request={})])
        r = self.run_script('sync_issues.sh')
        self.assertEqual(r.returncode, 0, r.stderr)
        files = list((self.work / 'reports/queue').glob('*.md'))
        self.assertEqual(len(files), 1)
        self.assertIn('-#1-', files[0].name)

    def test_foreign_author_issues_are_ignored(self):
        self.data(issues=[self.issue(1), dict(self.issue(2), user={'login': 'stranger'})])
        r = self.run_script('sync_issues.sh')
        self.assertEqual(r.returncode, 0, r.stderr)
        queued = list((self.work / 'reports/queue').glob('*.md'))
        self.assertEqual(len(queued), 1)
        self.assertIn('-#1-', queued[0].name)


if __name__ == '__main__':
    unittest.main()
