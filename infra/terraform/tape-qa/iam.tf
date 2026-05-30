// ── EC2 instance role: what the ephemeral QA box may do ────────────────────
// Read the release tarball + harness scripts; write its results back; be
// reachable via SSM Session Manager. No delete anywhere.
resource "aws_iam_role" "qa_box" {
  name = "bpt-tape-qa-box"
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { Service = "ec2.amazonaws.com" }
      Action    = "sts:AssumeRole"
    }]
  })
  tags = { Name = "bpt-tape-qa-box" }
}

// Session Manager (egress-based shell-in, no SSH/port 22).
resource "aws_iam_role_policy_attachment" "qa_box_ssm" {
  role       = aws_iam_role.qa_box.name
  policy_arn = "arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore"
}

resource "aws_iam_role_policy" "qa_box" {
  name = "qa-box-s3"
  role = aws_iam_role.qa_box.id
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Sid      = "ReadReleaseTarball"
        Effect   = "Allow"
        Action   = ["s3:GetObject", "s3:ListBucket"]
        Resource = [data.aws_s3_bucket.releases.arn, "${data.aws_s3_bucket.releases.arn}/*"]
      },
      {
        Sid      = "ReadHarnessWriteResults"
        Effect   = "Allow"
        Action   = ["s3:GetObject", "s3:ListBucket", "s3:PutObject"]
        Resource = [aws_s3_bucket.qa.arn, "${aws_s3_bucket.qa.arn}/*"]
      }
    ]
  })
}

resource "aws_iam_instance_profile" "qa_box" {
  name = "bpt-tape-qa-box"
  role = aws_iam_role.qa_box.name
}

// ── Step Functions execution role ──────────────────────────────────────────
resource "aws_iam_role" "sfn" {
  name = "bpt-tape-qa-sfn"
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { Service = "states.amazonaws.com" }
      Action    = "sts:AssumeRole"
    }]
  })
  tags = { Name = "bpt-tape-qa-sfn" }
}

resource "aws_iam_role_policy" "sfn" {
  name = "qa-sfn"
  role = aws_iam_role.sfn.id
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        // RunInstances/CreateTags can't be tightly resource-scoped; bound to
        // this region. Terminate is gated by the bpt_qa tag so SF can only
        // ever kill QA boxes, never the prod recorder.
        Sid       = "LaunchQaInstance"
        Effect    = "Allow"
        Action    = ["ec2:RunInstances", "ec2:CreateTags", "ec2:DescribeInstances"]
        Resource  = "*"
        Condition = { StringEquals = { "aws:RequestedRegion" = var.aws_region } }
      },
      {
        Sid       = "TerminateOnlyQaInstances"
        Effect    = "Allow"
        Action    = "ec2:TerminateInstances"
        Resource  = "*"
        Condition = { StringEquals = { "ec2:ResourceTag/bpt_qa" = "true" } }
      },
      {
        // Let RunInstances attach the QA box instance profile, nothing else.
        Sid       = "PassQaBoxRole"
        Effect    = "Allow"
        Action    = "iam:PassRole"
        Resource  = aws_iam_role.qa_box.arn
        Condition = { StringEquals = { "iam:PassedToService" = "ec2.amazonaws.com" } }
      },
      {
        Sid      = "ReadVerdict"
        Effect   = "Allow"
        Action   = "s3:GetObject"
        Resource = "${aws_s3_bucket.qa.arn}/verdict/*"
      },
      {
        Sid      = "NotifySns"
        Effect   = "Allow"
        Action   = "sns:Publish"
        Resource = aws_sns_topic.qa.arn
      }
    ]
  })
}

// ── SSM Automation role: the form's one privilege is to start the machine ───
resource "aws_iam_role" "ssm_automation" {
  name = "bpt-tape-qa-ssm"
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { Service = "ssm.amazonaws.com" }
      Action    = "sts:AssumeRole"
    }]
  })
  tags = { Name = "bpt-tape-qa-ssm" }
}

resource "aws_iam_role_policy" "ssm_automation" {
  name = "qa-ssm-start"
  role = aws_iam_role.ssm_automation.id
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect   = "Allow"
      Action   = "states:StartExecution"
      Resource = aws_sfn_state_machine.qa.arn
    }]
  })
}
