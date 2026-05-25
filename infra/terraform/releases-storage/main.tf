// Release tarball bucket. Layout:
//   s3://bpt-releases/<git-sha>/bpt-<version>.tar.gz
//   s3://bpt-releases/<git-sha>/MANIFEST       (text — version, sha, date)
//
// CI uploads here on every tag push; bpt-deploy fetches from here when
// shipping to a trading host. Old tarballs are cache, not record — the
// git tag is the source of truth; tarballs can always be rebuilt.

resource "aws_s3_bucket" "releases" {
  bucket = var.releases_bucket_name

  tags = {
    Name      = var.releases_bucket_name
    Role      = "releases"
    Env       = var.env_tag
    Owner     = var.owner_tag
    ManagedBy = "terraform-releases-storage"
  }
}

resource "aws_s3_bucket_versioning" "releases" {
  bucket = aws_s3_bucket.releases.id
  versioning_configuration {
    status = "Enabled"
  }
}

resource "aws_s3_bucket_server_side_encryption_configuration" "releases" {
  bucket = aws_s3_bucket.releases.id
  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

resource "aws_s3_bucket_public_access_block" "releases" {
  bucket                  = aws_s3_bucket.releases.id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

// Lifecycle: expire release tarballs after retain_days. Tags are immutable
// in git; if you need an older release, re-run package-release.sh against
// the tag. Versioning is on so accidental deletes during the window are
// recoverable.
resource "aws_s3_bucket_lifecycle_configuration" "releases" {
  bucket = aws_s3_bucket.releases.id

  rule {
    id     = "expire-old-releases"
    status = "Enabled"

    filter {}

    expiration {
      days = var.retain_days
    }

    noncurrent_version_expiration {
      noncurrent_days = 30
    }

    abort_incomplete_multipart_upload {
      days_after_initiation = 7
    }
  }
}

// Optional CI role grant. When ci_role_principal is set (a GitHub Actions
// OIDC role ARN), it gets PutObject on the bucket — letting the release
// workflow upload without long-lived access keys. Leave empty to manage
// principals out-of-band (e.g., via tape-iam-style explicit IAM users).
data "aws_iam_policy_document" "ci_put" {
  count = var.ci_role_principal == "" ? 0 : 1

  statement {
    sid    = "AllowCIPutObject"
    effect = "Allow"
    principals {
      type        = "AWS"
      identifiers = [var.ci_role_principal]
    }
    actions = [
      "s3:PutObject",
      "s3:PutObjectTagging",
      "s3:AbortMultipartUpload",
      "s3:ListBucket",
    ]
    resources = [
      aws_s3_bucket.releases.arn,
      "${aws_s3_bucket.releases.arn}/*",
    ]
  }
}

resource "aws_s3_bucket_policy" "ci_put" {
  count  = var.ci_role_principal == "" ? 0 : 1
  bucket = aws_s3_bucket.releases.id
  policy = data.aws_iam_policy_document.ci_put[0].json
}
