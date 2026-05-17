# TinkerTab docs site

Docusaurus 3 site for TinkerTab + TinkerBox + TinkerClaw.  Hosted on this
DGX device via Caddy at `http://tinker.local` (mDNS) and the raw LAN IP.

## Local development (live reload)

```bash
cd ~/projects/TinkerTab/docs-site
npm install              # first time only
npm run start -- --host 0.0.0.0 --port 3000
# → http://<LAN-IP>:3000  (or http://localhost:3000 from this device)
```

## Production build + serve

```bash
npm run build
sudo systemctl restart tinkertab-docs
curl -s http://localhost/ | head -c 80
```

## First-time install on the DGX device

```bash
# 1. Install Caddy + avahi (one-shot)
sudo apt install -y debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
  | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
  | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo apt update && sudo apt install -y caddy avahi-daemon
sudo systemctl disable --now caddy   # we use our own systemd unit

# 2. Install our systemd units (Caddy + tinker.local mDNS alias)
sudo cp deploy/tinkertab-docs.service /etc/systemd/system/
sudo cp deploy/tinker-local-alias.service /etc/systemd/system/
sudo cp deploy/tinkertab-docs.avahi.service /etc/avahi/services/
sudo systemctl daemon-reload
sudo systemctl reload avahi-daemon

# 3. Build + start
npm install && npm run build
sudo systemctl enable --now tinkertab-docs tinker-local-alias

# 4. Verify
curl -s http://localhost/ | grep TinkerTab
avahi-resolve -n tinker.local            # → tinker.local <LAN-IP>
# → open http://tinker.local/ from any device on the LAN
```

The `tinker-local-alias` unit runs `avahi-publish-address tinker.local <LAN-IP>`
in the foreground, so the alias survives DHCP renewals and reboots.  If your
device hostname is already named `tinker` you can skip the alias unit entirely
— `tinker.local` will resolve via the normal avahi hostname publication.

## Sync TinkerBox docs (for PR 3 curation)

```bash
./scripts/sync-tinkerbox.sh
# → drops ~/projects/TinkerBox/docs/ into ./imported/tinkerbox/ (gitignored)
```

## Project structure

```
docs-site/
├── docusaurus.config.ts   # site config
├── sidebars.ts            # information architecture
├── src/
│   ├── css/custom.css     # emerald-accent theme tokens
│   ├── components/
│   │   ├── HeroBanner/    # custom home hero
│   │   └── FeatureCards/  # 3×2 feature grid
│   └── pages/index.tsx    # home page
├── docs/                  # the actual content tree (50+ pages)
├── static/img/            # logos + hero device + screenshots
├── deploy/                # Caddyfile, systemd unit, avahi service
└── scripts/               # sync helpers
```

## Reference

Source-of-truth plan: `~/.claude/plans/jolly-fluttering-raven.md`.
