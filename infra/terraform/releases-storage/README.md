# Releases storage

S3 bucket holding deploy tarballs built by CI. Companion to `tape-storage/`
but with a different lifecycle (tarballs expire; tape data forever).

## What lives here

```
s3://bpt-releases/<git-sha>/bpt-<version>.tar.gz
s3://bpt-releases/<git-sha>/MANIFEST
```

Sha-prefixed so multiple builds of the same tag (e.g., re-run after fixing
a CI flake) don't clobber. The MANIFEST file is small text with version,
sha, build timestamp, and the list of binaries in the tarball.

## First-time apply

```bash
cd infra/terraform/releases-storage
terraform init
terraform apply
```

After apply, terraform prints the bucket name. Wire it into:

1. CI release workflow — `BPT_RELEASES_BUCKET` env var
2. Trading host IAM policy — `s3:GetObject` on `${releases_bucket_arn}/*`
3. `deploy/bpt-deploy` wrapper — `BPT_RELEASES_BUCKET` env var

## Variables

| name | default | purpose |
|---|---|---|
| `aws_region` | `ap-southeast-1` | Same region as trading host (free intra-region transfer) |
| `releases_bucket_name` | `bpt-releases` | S3 bucket name (must be globally unique) |
| `retain_days` | `90` | Auto-expire tarballs after N days |
| `ci_role_principal` | `""` | If set, grants PutObject to this IAM role (typically a GitHub Actions OIDC role) |

## What this module does NOT do

- Does not create the GitHub Actions IAM role itself. That's a separate
  identity concern, lives in `tape-iam/` or a sibling `ci-iam/` module.
- Does not attach the trading host's read permission. That belongs on the
  trading host's instance role in `trading-host/`.
