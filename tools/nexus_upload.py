"""Mouse & Window Fixes' Nexus files (mod 4112), through the Nexus Mods v3 API. The first run creates the two files
(main: MouseWindowFixes-<version>.zip, the Mod Manager Download default; miscellaneous: the source zip); later runs add
each zip as a new version of its file, archiving the old one. The changelog for the version comes from
nexus/changelog_<version>.txt.

The API key: NEXUSMODS_API_KEY, else the first line of %USERPROFILE%\\.nexusmods_api_key (never in the project).

nexus_upload.py            shows what it would do (nothing is sent)
nexus_upload.py --upload   does it (asks once more before sending)

The page itself (description, images, summary) stays manual: the API can't edit it."""
import json, os, re, sys, time, urllib.error, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
API = 'https://api.nexusmods.com/v3'
GAME, MOD_ID = 'crimsondesert', '4112'
# zip suffix -> (file name on Nexus, category for a new file, the Mod Manager Download default, file description)
FILES = {
    '': ('Mouse and Window Fixes', 'main', True,
         'The mod: MouseWindowFixes.asi, with its licences. Add it to DMM as an ASI mod, or put it in bin64.'),
    '-source': ('Mouse and Window Fixes - Source code', 'miscellaneous', False,
                'The source code (C++, CMake). Also on GitHub: https://github.com/Yorivel/cd-mouse-window-fixes'),
}


def api_key():
    k = os.environ.get('NEXUSMODS_API_KEY', '').strip()
    p = os.path.join(os.path.expanduser('~'), '.nexusmods_api_key')
    if not k and os.path.exists(p):
        k = open(p, encoding='utf-8').readline().strip()
    if not k:
        sys.exit('No API key: set NEXUSMODS_API_KEY or put it in %USERPROFILE%\\.nexusmods_api_key')
    return k


def call(method, path, body=None, key=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API + path, data=data, method=method,
                                 headers={'apikey': key, 'Content-Type': 'application/json', 'Accept': 'application/json',
                                          'User-Agent': 'MouseWindowFixes-upload/1.0'})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            txt = r.read().decode()
            return json.loads(txt) if txt else {}
    except urllib.error.HTTPError as e:
        sys.exit(f'{method} {path}: HTTP {e.code}\n{e.read().decode(errors="replace")[:2000]}')


def put_part(url, chunk):
    req = urllib.request.Request(url, data=chunk, method='PUT', headers={'Content-Type': 'application/octet-stream',
                                                                         'Content-Length': str(len(chunk))})
    with urllib.request.urlopen(req, timeout=300) as r:
        etag = r.headers.get('ETag')
    if not etag:
        sys.exit('a part was sent without an ETag back')
    return etag.replace('"', '')


def upload(path, key):
    """The zip as a finished, scanned upload; its id."""
    size = os.path.getsize(path)
    d = call('POST', '/uploads/multipart', {'filename': os.path.basename(path), 'size_bytes': str(size)}, key)['data']
    uid, urls, part = d['id'], d['part_presigned_urls'], int(d['part_size_bytes'])
    parts = []
    with open(path, 'rb') as f:
        for n, url in enumerate(urls, 1):
            parts.append((n, put_part(url, f.read(part))))
    xml = '<CompleteMultipartUpload>\n' + '\n'.join(
        f'  <Part>\n    <PartNumber>{n}</PartNumber>\n    <ETag>{e}</ETag>\n  </Part>' for n, e in parts) + \
        '\n</CompleteMultipartUpload>'
    req = urllib.request.Request(d['complete_presigned_url'], data=xml.encode(), method='POST',
                                 headers={'Content-Type': 'application/xml'})
    urllib.request.urlopen(req, timeout=120).close()
    call('POST', f'/uploads/{uid}/finalise', key=key)
    delay = 2.0
    for _ in range(60):  # (Nexus scans the file first)
        state = call('GET', f'/uploads/{uid}', key=key)['data']['state']
        print(f'  upload {uid}: {state}')
        if state == 'available':
            return uid
        time.sleep(delay)
        delay = min(delay * 1.5, 30)
    sys.exit(f'upload {uid} never became available')


def main():
    key = api_key()
    go = '--upload' in sys.argv
    rc = open(os.path.join(ROOT, 'src', 'mouse_window_fixes.rc'), encoding='utf-8').read()
    ver = re.search(r'#define MWF_VERSION_STR "([^"]+)"', rc).group(1)
    changelog = open(os.path.join(ROOT, 'nexus', f'changelog_{ver}.txt'), encoding='utf-8').read().strip()
    mod = call('GET', f'/games/{GAME}/mods/{MOD_ID}', key=key)['data']
    print(f'mod {MOD_ID}: {mod.get("name")} (id {mod["id"]}, version {mod.get("version")}, {mod.get("status")})')
    files = call('GET', f'/mods/{mod["id"]}/files', key=key)['data']['mod_files']
    for f in files:
        vs = call('GET', f'/mod-files/{f["id"]}/versions', key=key)['data']['versions']
        live = [v for v in vs if v['category'] != 'archived']
        f['latest'] = max(live, key=lambda v: v['uploaded_at']) if live else None
    plan = []
    for suffix, (name, category, mm, desc) in FILES.items():
        zip_path = os.path.join(ROOT, 'release', f'MouseWindowFixes-{ver}{suffix}.zip')
        if not os.path.exists(zip_path):
            sys.exit(f'missing {zip_path}: run tools/pack_release.py')
        match = [f for f in files if f.get('name') == name]
        plan.append((zip_path, name, category, mm, desc, match[0] if match else None))
    print(f'\nversion {ver}; changelog:\n  ' + changelog.replace('\n', '\n  '))
    for zip_path, name, category, mm, desc, f in plan:
        what = (f'new version of file {f["id"]}, replacing {f["latest"]["version"]} (archived)' if f and f['latest']
                else f'NEW file, category {category}')
        print(f'\n{os.path.basename(zip_path)} ({os.path.getsize(zip_path) // 1024} KB) -> "{name}": {what}'
              + ('; the Mod Manager Download default' if mm else '') + f'\n  description: {desc}')
    if not go:
        print('\nNothing sent (run with --upload to do it).')
        return
    if input('\nUpload these to Nexus now? Type "yes": ').strip().lower() != 'yes':
        sys.exit('not uploaded')
    for zip_path, name, category, mm, desc, f in plan:
        print(f'\nuploading {os.path.basename(zip_path)}')
        uid = upload(zip_path, key)
        body = {'upload_id': uid, 'name': name, 'version': ver, 'description': desc, 'update_mod_version': mm,
                'allow_mod_manager_download': True, 'primary_mod_manager_download': mm}
        if f and f['latest']:
            body.update(file_category=f['latest']['category'], previous_version_id=f['latest']['id'],
                        archive_existing_file=True)
            r = call('POST', f'/mod-files/{f["id"]}/versions', body, key)['data']
        else:
            body.update(mod_id=mod['id'], file_category=category)
            r = call('POST', '/mod-files', body, key)['data']
        print(f'  done: {json.dumps(r)[:300]}')
    call('POST', f'/mods/{mod["id"]}/changelogs', {'version': ver, 'changelog': changelog}, key)
    print('\nchangelog added; done')


if __name__ == '__main__':
    main()
