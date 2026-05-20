# ──────────────────────── ECR repository ──────────────────────────────
# Holds the Lambda container image. Pushed by deploy.sh.

resource "aws_ecr_repository" "refresh" {
  name                 = "${var.name_prefix}-refresh"
  image_tag_mutability = "MUTABLE" # 'latest' rolls per deploy

  image_scanning_configuration {
    scan_on_push = true # free Inspector basic scan
  }
}

# Keep only the 10 most-recent untagged images + every tagged image.
# Prevents the repo from growing unbounded across deploys.
resource "aws_ecr_lifecycle_policy" "refresh" {
  repository = aws_ecr_repository.refresh.name

  policy = jsonencode({
    rules = [{
      rulePriority = 1
      description  = "Keep last 10 untagged images"
      selection = {
        tagStatus   = "untagged"
        countType   = "imageCountMoreThan"
        countNumber = 10
      }
      action = { type = "expire" }
    }]
  })
}

# ──────────────────────── IAM role + policies ─────────────────────────

data "aws_iam_policy_document" "lambda_assume" {
  statement {
    effect  = "Allow"
    actions = ["sts:AssumeRole"]
    principals {
      type        = "Service"
      identifiers = ["lambda.amazonaws.com"]
    }
  }
}

resource "aws_iam_role" "lambda" {
  name               = "${var.name_prefix}-refresh"
  assume_role_policy = data.aws_iam_policy_document.lambda_assume.json
}

# Basic execution role — CloudWatch Logs write access.
resource "aws_iam_role_policy_attachment" "lambda_basic" {
  role       = aws_iam_role.lambda.name
  policy_arn = "arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole"
}

# Read the DB secret at runtime.
data "aws_iam_policy_document" "lambda_secret_read" {
  statement {
    effect    = "Allow"
    actions   = ["secretsmanager:GetSecretValue"]
    resources = [aws_secretsmanager_secret.db.arn]
  }
}

resource "aws_iam_policy" "lambda_secret_read" {
  name   = "${var.name_prefix}-refresh-secret-read"
  policy = data.aws_iam_policy_document.lambda_secret_read.json
}

resource "aws_iam_role_policy_attachment" "lambda_secret_read" {
  role       = aws_iam_role.lambda.name
  policy_arn = aws_iam_policy.lambda_secret_read.arn
}

# Upload the rendered instrument_mapping.json snapshot to S3 after each
# successful refresh. Scoped to the one object key, not the whole bucket.
data "aws_iam_policy_document" "lambda_snapshot_upload" {
  statement {
    effect    = "Allow"
    actions   = ["s3:PutObject", "s3:PutObjectAcl"]
    resources = ["arn:aws:s3:::${var.snapshot_s3_bucket}/${var.snapshot_s3_key}"]
  }
}

resource "aws_iam_policy" "lambda_snapshot_upload" {
  name   = "${var.name_prefix}-refresh-snapshot-upload"
  policy = data.aws_iam_policy_document.lambda_snapshot_upload.json
}

resource "aws_iam_role_policy_attachment" "lambda_snapshot_upload" {
  role       = aws_iam_role.lambda.name
  policy_arn = aws_iam_policy.lambda_snapshot_upload.arn
}

# ──────────────────────── CloudWatch log group ────────────────────────
# Pre-created with retention so logs don't accumulate forever (AWS
# default is 'never expire' → unbounded cost growth).

resource "aws_cloudwatch_log_group" "lambda" {
  name              = "/aws/lambda/${var.name_prefix}-refresh"
  retention_in_days = var.log_retention_days
}

# ──────────────────────── Lambda function ─────────────────────────────

resource "aws_lambda_function" "refresh" {
  function_name = "${var.name_prefix}-refresh"
  role          = aws_iam_role.lambda.arn

  package_type = "Image"
  image_uri    = "${aws_ecr_repository.refresh.repository_url}:${var.lambda_image_tag}"

  memory_size = var.lambda_memory_mb
  timeout     = var.lambda_timeout_seconds

  environment {
    variables = {
      SECMASTER_DB_SECRET_ARN     = aws_secretsmanager_secret.db.arn
      SECMASTER_DISCORD_WEBHOOK   = var.discord_webhook_url
      SECMASTER_SNAPSHOT_S3_URI   = "s3://${var.snapshot_s3_bucket}/${var.snapshot_s3_key}"
      # SECMASTER_VENUES left unset — handler defaults to all venues.
    }
  }

  depends_on = [
    aws_cloudwatch_log_group.lambda,
    aws_iam_role_policy_attachment.lambda_basic,
    aws_iam_role_policy_attachment.lambda_secret_read,
    aws_iam_role_policy_attachment.lambda_snapshot_upload,
  ]

  lifecycle {
    # Image tag changes from deploy.sh shouldn't trigger drift —
    # explicit `terraform apply` after a deploy will catch up.
    ignore_changes = [image_uri]
  }
}

# ──────────────────────── EventBridge schedule ────────────────────────

resource "aws_cloudwatch_event_rule" "daily" {
  name                = "${var.name_prefix}-refresh-daily"
  description         = "Fires the secmaster refresh Lambda daily"
  schedule_expression = var.lambda_schedule_cron
}

resource "aws_cloudwatch_event_target" "daily" {
  rule = aws_cloudwatch_event_rule.daily.name
  arn  = aws_lambda_function.refresh.arn
}

resource "aws_lambda_permission" "eventbridge_invoke" {
  statement_id  = "AllowEventBridgeInvoke"
  action        = "lambda:InvokeFunction"
  function_name = aws_lambda_function.refresh.function_name
  principal     = "events.amazonaws.com"
  source_arn    = aws_cloudwatch_event_rule.daily.arn
}
