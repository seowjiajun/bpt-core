output "releases_bucket_name" {
  description = "Name of the releases S3 bucket. Use as BPT_RELEASES_BUCKET in CI + deploy tooling."
  value       = aws_s3_bucket.releases.id
}

output "releases_bucket_arn" {
  description = "ARN of the releases bucket. Attach to IAM policies that need s3:GetObject for fetching tarballs (e.g., the trading host's instance role)."
  value       = aws_s3_bucket.releases.arn
}

output "releases_bucket_region" {
  description = "Region the bucket lives in. Pass to aws-cli --region when fetching."
  value       = aws_s3_bucket.releases.region
}
