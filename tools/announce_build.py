"""Tells Discord a build is out (CI's announce job, once both jobs published it).

Two webhooks, both optional (GitHub repo > Settings > Secrets and variables > Actions):
  DISCORD_BUILDS_WEBHOOK  "HomeNX build N is out", with the downloads
  DISCORD_NEWS_WEBHOOK    the update log: what changed since the last build
Neither set: nothing is sent.
"""
import json
import os
import subprocess
import urllib.request

REPO = os.environ.get("GITHUB_REPOSITORY", "ad3333m/homenx")
BUILD = os.environ.get("HOMENX_BUILD", "?")
BLUE = 0x5A6EDC
NO_PINGS = {"parse": []}


def run(*args):
    return subprocess.run(args, capture_output=True, text=True).stdout.strip()


def changes():
    # Since the build before this one: this workflow's newest successful run on master
    # (this run hasn't finished, so it's the last one).
    prev = run("gh", "run", "list", "-R", REPO, "--workflow", "switch.yml", "--branch", "master",
               "--status", "success", "--limit", "1", "--json", "headSha", "-q", ".[0].headSha")
    span = [prev + "..HEAD"] if prev and run("git", "cat-file", "-t", prev) == "commit" else ["-10"]
    titles = run("git", "log", "--no-merges", "--pretty=format:%s", *span).splitlines()
    return [t for t in titles if t and not t.startswith("Revert")]


def post(hook, embed):
    body = json.dumps({"username": "HomeNX", "allowed_mentions": NO_PINGS, "embeds": [embed]}).encode()
    req = urllib.request.Request(hook, data=body, headers={"Content-Type": "application/json",
                                                           "User-Agent": "HomeNX-CI"})
    urllib.request.urlopen(req, timeout=20).read()


def main():
    builds, news = os.environ.get("BUILDS_HOOK", ""), os.environ.get("NEWS_HOOK", "")
    if not builds and not news:
        print("No Discord webhooks set: nothing announced.")
        return
    if builds:
        post(builds, {
            "title": f"HomeNX build {BUILD} is out",
            "url": f"https://github.com/{REPO}/releases/tag/latest",
            "color": BLUE,
            "description": "\n".join([
                f"[HomeNX-full.zip](https://github.com/{REPO}/releases/download/latest/HomeNX-full.zip): a fresh install",
                f"[CardsNX.nro](https://github.com/{REPO}/releases/download/cardsnx/CardsNX.nro)",
                "Already installed? Update in NXToggle, or Settings > Updates.",
            ]),
        })
    if news:
        text = "\n".join("• " + t for t in changes()) or "Small fixes."
        if len(text) > 3900:  # (Discord's limit is 4096)
            text = text[:3900].rsplit("\n", 1)[0] + "\n…"
        post(news, {"title": f"What's new in build {BUILD}", "color": BLUE, "description": text})
    print("Announced build", BUILD)


if __name__ == "__main__":
    main()
