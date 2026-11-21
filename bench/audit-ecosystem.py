#!/usr/bin/env python3
# Audit of the ecosystem entries of lz4.org (ports, CLIs, decoders, users) :
# link status, whether the linked page mentions lz4, last activity of the project (repository or package registry).
# usage : audit-ecosystem.py index.html audit.csv
# Needs curl and gh (GitHub CLI, logged in). A proxy, if any, comes from the environment (https_proxy).
# 403 / 429 are often anti-bot answers : check those links in a browser.
import concurrent.futures, csv, html as html_lib, json, os, re, subprocess, sys, tempfile, urllib.parse

ENV = os.environ
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"

html = open(sys.argv[1], encoding="utf-8").read()
xmp = html[html.index("<a name=\"interoperable-lz4\">"):html.index("### Special Thanks")]

entries = []
section = ""
for line in xmp.splitlines():
    m = re.match(r"#{2,4}\s+(.*)", line)
    if m:
        section = m.group(1).strip(); continue
    if line.startswith("|__"):
        f = [x.strip() for x in line.split("|")]
        url = re.search(r"https?://\S+", f[3]).group(0)
        entries.append(dict(section=section, name=f[1].replace("__", ""), author=f[2], url=url, logo=""))
        continue
    m = re.match(r"<h3>(.*)</h3>", line)
    if m:
        section = m.group(1).strip(); continue
    m = re.search(r'<div class="usedby-entry"><img alt="" src="([^"]+)"><div><a class="usedby-name" href="([^"]+)">([^<]+)</a>', line)
    if m:
        entries.append(dict(section=section, name=m.group(3), author="", url=html_lib.unescape(m.group(2)), logo=m.group(1)))

def fetch(url):
    with tempfile.NamedTemporaryFile() as body:
        r = subprocess.run(["curl", "-sS", "-L", "--max-time", "30", "--max-filesize", "20000000", "-A", UA,
                            "-o", body.name, "-w", "%{http_code} %{url_effective}", url],
                           capture_output=True, text=True, env=ENV)
        code, _, final = r.stdout.partition(" ")
        text = open(body.name, "rb").read().decode("utf-8", "replace")
        err = r.stderr.strip().splitlines()[-1] if r.returncode else ""
        return code, final, ("yes" if re.search(r"lz4", text, re.I) else "no"), err

def gh(path):
    r = subprocess.run(["gh", "api", path], capture_output=True, text=True, env=ENV)
    return json.loads(r.stdout) if r.returncode == 0 else None

def activity(url):
    """last activity of the project (repository or package registry) : (date, stars, note)"""
    u = urllib.parse.urlparse(url)
    p = [x for x in u.path.split("/") if x]
    try:
        if u.netloc == "github.com" and len(p) >= 2:
            d = gh("repos/%s/%s" % (p[0], p[1]))
            if not d: return "", "", "repo not found"
            c = gh("repos/%s/commits?per_page=1" % d["full_name"])
            last = c[0]["commit"]["committer"]["date"][:10] if c else ""
            note = []
            if d["archived"]: note.append("archived")
            if d["full_name"].lower() != ("%s/%s" % (p[0], p[1])).lower(): note.append("moved to " + d["full_name"])
            return last, str(d["stargazers_count"]), " ; ".join(note)
        def getjson(api):
            r = subprocess.run(["curl", "-sS", "-L", "--max-time", "30", api], capture_output=True, text=True, env=ENV)
            return json.loads(r.stdout)
        def gettext(api):
            return subprocess.run(["curl", "-sS", "-L", "--max-time", "30", api], capture_output=True, text=True, env=ENV).stdout.strip()
        if u.netloc == "pypi.org":
            d = getjson("https://pypi.org/pypi/%s/json" % p[1])
            times = [f["upload_time"] for files in d["releases"].values() for f in files]
            return max(times)[:10], "", "pypi " + d["info"]["version"]
        if u.netloc == "metacpan.org":
            d = getjson("https://fastapi.metacpan.org/v1/release/%s" % p[1])
            return d["date"][:10], "", "cpan " + str(d["version"])
        if u.netloc == "pub.dev":
            name = p[1] if p[0] == "packages" else p[1]
            d = getjson("https://pub.dev/api/packages/%s" % name)
            return d["latest"]["published"][:10], "", "pub " + d["latest"]["version"]
        if u.netloc == "hackage.haskell.org":
            t = gettext("https://hackage.haskell.org/package/%s/upload-time" % p[1])
            return t[:10] if re.match(r"\d{4}", t) else "", "", "hackage"
        if u.netloc == "rubygems.org":
            d = getjson("https://rubygems.org/api/v1/versions/%s.json" % p[1])
            return d[0]["created_at"][:10], "", "gem " + d[0]["number"]
        if u.netloc == "code.dlang.org":
            d = getjson("https://code.dlang.org/api/packages/%s/info" % p[1])
            dates = [v.get("date", "") for v in d.get("versions", [])]
            return max(dates)[:10] if dates else "", "", "dub"
        if u.netloc == "gitlab.com":
            d = getjson("https://gitlab.com/api/v4/projects/%s" % urllib.parse.quote("/".join(p[:2]), safe=""))
            return d["last_activity_at"][:10], str(d.get("star_count", "")), "gitlab" + (" archived" if d.get("archived") else "")
        if u.netloc == "codeberg.org":
            d = getjson("https://codeberg.org/api/v1/repos/%s/%s" % (p[0], p[1]))
            return d["updated_at"][:10], str(d.get("stars_count", "")), "codeberg" + (" archived" if d.get("archived") else "")
    except Exception as e:
        return "", "", "activity error : %s" % e
    return "", "", ""

def audit(e):
    e["status"], e["final_url"], e["mentions_lz4"], e["error"] = fetch(e["url"])
    e["last_activity"], e["stars"], e["note"] = activity(e["url"])
    if e["logo"]:
        path = os.path.join(os.path.dirname(sys.argv[1]), e["logo"])
        e["logo"] = e["logo"] + ("" if os.path.exists(path) else " (missing)")
    return e

with concurrent.futures.ThreadPoolExecutor(12) as pool:
    results = list(pool.map(audit, entries))

keys = ["section", "name", "author", "url", "status", "final_url", "mentions_lz4", "last_activity", "stars", "note", "logo", "error"]
w = csv.DictWriter(open(sys.argv[2], "w", newline=""), fieldnames=keys)
w.writeheader()
for e in results: w.writerow(e)
print(len(results), "entries")
