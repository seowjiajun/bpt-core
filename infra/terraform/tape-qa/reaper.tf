// Reaper layer 2: a daily Lambda sweep that terminates any orphaned QA box
// (bpt_qa=true) older than MAX_AGE_MIN. Independent of the box + Step
// Functions, so it catches what those miss (boot failure, cancelled shutdown,
// SF failure mid-flight). EventBridge Scheduler invokes it once a day.

data "archive_file" "reaper" {
  type        = "zip"
  source_file = "${path.module}/../../../scripts/qa/reaper_lambda.py"
  output_path = "${path.module}/reaper_lambda.zip"
}

resource "aws_iam_role" "reaper" {
  name = "bpt-tape-qa-reaper"
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { Service = "lambda.amazonaws.com" }
      Action    = "sts:AssumeRole"
    }]
  })
  tags = { Name = "bpt-tape-qa-reaper" }
}

resource "aws_iam_role_policy_attachment" "reaper_logs" {
  role       = aws_iam_role.reaper.name
  policy_arn = "arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole"
}

resource "aws_iam_role_policy" "reaper" {
  name = "qa-reaper"
  role = aws_iam_role.reaper.id
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Sid      = "FindQaInstances"
        Effect   = "Allow"
        Action   = "ec2:DescribeInstances"
        Resource = "*"
      },
      {
        Sid       = "TerminateOnlyQaInstances"
        Effect    = "Allow"
        Action    = "ec2:TerminateInstances"
        Resource  = "*"
        Condition = { StringEquals = { "ec2:ResourceTag/bpt_qa" = "true" } }
      }
    ]
  })
}

resource "aws_lambda_function" "reaper" {
  function_name    = "bpt-tape-qa-reaper"
  role             = aws_iam_role.reaper.arn
  runtime          = "python3.12"
  handler          = "reaper_lambda.handler"
  filename         = data.archive_file.reaper.output_path
  source_code_hash = data.archive_file.reaper.output_base64sha256
  timeout          = 60
  environment {
    variables = { MAX_AGE_MIN = "600" }
  }
  tags = { Name = "bpt-tape-qa-reaper" }
}

// Scheduler → Lambda (role with InvokeFunction), mirroring the trading-host
// auto-stop schedule's direct-target pattern.
resource "aws_iam_role" "reaper_schedule" {
  name = "bpt-tape-qa-reaper-sched"
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { Service = "scheduler.amazonaws.com" }
      Action    = "sts:AssumeRole"
    }]
  })
}

resource "aws_iam_role_policy" "reaper_schedule" {
  name = "invoke-reaper"
  role = aws_iam_role.reaper_schedule.id
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect   = "Allow"
      Action   = "lambda:InvokeFunction"
      Resource = aws_lambda_function.reaper.arn
    }]
  })
}

resource "aws_scheduler_schedule" "reaper" {
  name                         = "bpt-tape-qa-reaper"
  schedule_expression          = "cron(30 17 * * ? *)" // 17:30 UTC daily
  schedule_expression_timezone = "UTC"
  flexible_time_window {
    mode = "OFF"
  }
  target {
    arn      = aws_lambda_function.reaper.arn
    role_arn = aws_iam_role.reaper_schedule.arn
  }
}
