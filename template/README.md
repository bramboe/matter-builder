# A Matter Builder package source

Firmware and recipes for [Matter Builder](https://github.com/bramboe/matter-builder),
built and signed by this repository. Users add it in the add-on (Settings →
Package sources, or from the catalog); from then on the add-on only installs
firmware from here that is signed with this source's key.

## Set up

1. Make a repository from this template (public).
2. Make a key pair, keep the private half secret:

   ```sh
   openssl ecparam -name prime256v1 -genkey -noout -out signing-key.pem
   openssl ec -in signing-key.pem -pubout -out matter-builder-key.pem
   ```

   Commit `matter-builder-key.pem` (the public half) at the root. Store the
   whole of `signing-key.pem` as the repository secret `MB_SIGNING_KEY`, then
   delete it from the repository folder (`.gitignore` keeps `*.pem` out, except
   the public key).
3. Give the repository the topic `matter-builder-package`, so the catalog
   finds it (it is rebuilt every night).

## Contents

| Path | What |
|---|---|
| `packages/<name>/` | Firmware: an ESP-IDF project on `mb_core` with a `package.yaml` (see `packages/README.md`) |
| `recipes/*.yaml` | Recipes: a generic device's connections, no code |
| `components/<type>/` | Components: a connection type of its own for the generic firmware (`component.yaml` + an ESP-IDF component on `mb_driver`; see Matter Builder's `examples/components/pulse_relay`). Others use one with `source: github:<you>/<repo>/components/<type>@<tag>` |
| `matter-builder-key.pem` | The public key the add-on pins |
| `.github/workflows/release.yml` | Builds, signs and publishes for a tag `v<version>` |

A source with only recipes needs no key and no workflow: recipes are
connections, not code. In Matter Builder,
*Share* on a configuration (Configs) makes such a repository with you: it
opens GitHub with the recipe filled in. Add a `# Summary: ...` comment with
one short sentence to describe what the recipe does in the catalog.

## Release

Tag a version: `git tag v0.1.0 && git push origin v0.1.0`. The workflow builds
every package, signs the manifest and publishes the release; the catalog picks
it up the next night.

Never put Wi-Fi details, passwords or other personal data in a package or a
recipe: users set those on the device itself.
