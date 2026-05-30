// ── AMI: same Ubuntu 24.04 as the prod recorder ────────────────────────────
data "aws_ami" "ubuntu" {
  most_recent = true
  owners      = [var.ami_owner]
  filter {
    name   = "name"
    values = [var.ami_name_pattern]
  }
  filter {
    name   = "virtualization-type"
    values = ["hvm"]
  }
  filter {
    name   = "architecture"
    values = ["x86_64"]
  }
}

data "aws_s3_bucket" "releases" {
  bucket = var.releases_bucket_name
}

// ── Minimal self-contained network ─────────────────────────────────────────
// Egress-only: the box reaches venue WS + S3 + Loki + ntfy + SSM endpoints
// outbound; nothing connects in (SSM Session Manager is egress-based, no SSH).
resource "aws_vpc" "qa" {
  cidr_block           = "10.43.0.0/16"
  enable_dns_hostnames = true
  enable_dns_support   = true
  tags                 = { Name = "bpt-tape-qa-vpc" }
}

resource "aws_internet_gateway" "qa" {
  vpc_id = aws_vpc.qa.id
  tags   = { Name = "bpt-tape-qa-igw" }
}

resource "aws_subnet" "qa" {
  vpc_id                  = aws_vpc.qa.id
  cidr_block              = "10.43.1.0/24"
  availability_zone       = var.availability_zone
  map_public_ip_on_launch = true
  tags                    = { Name = "bpt-tape-qa-subnet" }
}

resource "aws_route_table" "qa" {
  vpc_id = aws_vpc.qa.id
  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.qa.id
  }
  tags = { Name = "bpt-tape-qa-rt" }
}

resource "aws_route_table_association" "qa" {
  subnet_id      = aws_subnet.qa.id
  route_table_id = aws_route_table.qa.id
}

resource "aws_security_group" "qa" {
  name        = "bpt-tape-qa-sg"
  description = "QA recorder: egress all, no ingress (SSM-based access)."
  vpc_id      = aws_vpc.qa.id
  egress {
    description = "All egress (venue WS + S3 + Loki + ntfy + SSM)"
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
  tags = { Name = "bpt-tape-qa-sg" }
}

// ── QA bucket: persistent, per-run prefixes, lifecycle-expired ─────────────
resource "aws_s3_bucket" "qa" {
  bucket = var.qa_bucket_name
  tags   = { Name = "bpt-tape-qa" }
}

resource "aws_s3_bucket_public_access_block" "qa" {
  bucket                  = aws_s3_bucket.qa.id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket_server_side_encryption_configuration" "qa" {
  bucket = aws_s3_bucket.qa.id
  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

resource "aws_s3_bucket_lifecycle_configuration" "qa" {
  bucket = aws_s3_bucket.qa.id
  rule {
    id     = "expire-qa-data"
    status = "Enabled"
    filter {}
    expiration {
      days = var.qa_retention_days
    }
    abort_incomplete_multipart_upload {
      days_after_initiation = 1
    }
  }
}

// ── Harness scripts: shipped to the QA bucket, pulled by the box bootstrap ──
// Versioned independently of the release tarball so we can iterate the harness
// without rebuilding releases. NOTE: these files are written in the next slice;
// `terraform apply` will fail until scripts/qa/*.sh exist.
resource "aws_s3_object" "on_box_run" {
  bucket = aws_s3_bucket.qa.id
  key    = "harness/on_box_run.sh"
  source = "${path.module}/../../../scripts/qa/on_box_run.sh"
  etag   = filemd5("${path.module}/../../../scripts/qa/on_box_run.sh")
}

resource "aws_s3_object" "validate_tape_run" {
  bucket = aws_s3_bucket.qa.id
  key    = "harness/validate_tape_run.sh"
  source = "${path.module}/../../../scripts/qa/validate_tape_run.sh"
  etag   = filemd5("${path.module}/../../../scripts/qa/validate_tape_run.sh")
}

// ── Notifications: orchestration-level safety net (dead box / timeout) ──────
resource "aws_sns_topic" "qa" {
  name = "bpt-tape-qa"
}

resource "aws_sns_topic_subscription" "qa_email" {
  topic_arn = aws_sns_topic.qa.arn
  protocol  = "email"
  endpoint  = var.notify_email
}

// ── Launch template for the ephemeral QA box ───────────────────────────────
resource "aws_launch_template" "qa" {
  name_prefix   = "bpt-tape-qa-"
  image_id      = data.aws_ami.ubuntu.id
  instance_type = var.default_instance_type

  iam_instance_profile {
    name = aws_iam_instance_profile.qa_box.name
  }

  // Reaper layer 1: the box's `shutdown -h +TTL` (bootstrap) terminates it if
  // a run overruns or SF never tears it down. Needs terminate-on-shutdown.
  instance_initiated_shutdown_behavior = "terminate"

  network_interfaces {
    device_index                = 0
    associate_public_ip_address = true
    subnet_id                   = aws_subnet.qa.id
    security_groups             = [aws_security_group.qa.id]
  }

  // IMDSv2 required + instance tags exposed in metadata so the box reads its
  // per-run params (qa_run_id, qa_duration_min, …) without user_data juggling.
  metadata_options {
    http_endpoint          = "enabled"
    http_tokens            = "required"
    instance_metadata_tags = "enabled"
  }

  user_data = base64encode(templatefile("${path.module}/bootstrap.sh.tftpl", {
    releases_bucket = var.releases_bucket_name
    qa_region       = var.aws_region
    loki_url        = var.loki_url
    ntfy_url        = var.ntfy_url
  }))

  tag_specifications {
    resource_type = "instance"
    tags          = { Name = "bpt-tape-qa", bpt_qa = "true" }
  }
}

// ── Step Functions: the orchestrator ───────────────────────────────────────
resource "aws_sfn_state_machine" "qa" {
  name     = "bpt-tape-qa"
  role_arn = aws_iam_role.sfn.arn
  definition = templatefile("${path.module}/statemachine.asl.json.tftpl", {
    launch_template_id = aws_launch_template.qa.id
    qa_bucket          = aws_s3_bucket.qa.id
    sns_topic_arn      = aws_sns_topic.qa.arn
  })
}

// ── SSM Automation document: the parameterized console "form" ───────────────
// String params with allowedValues render as dropdowns in
// Systems Manager → Automation → Execute automation. Its one job is to start
// the state machine, using the automation execution id as the run_id.
resource "aws_ssm_document" "qa" {
  name            = "bpt-tape-qa"
  document_type   = "Automation"
  document_format = "JSON"

  content = jsonencode({
    schemaVersion = "0.3"
    description   = "Trigger an ephemeral bpt-tape QA run (Step Functions). Pick a duration/rotation and Execute."
    assumeRole    = aws_iam_role.ssm_automation.arn
    parameters = {
      duration_minutes = {
        type          = "String"
        default       = "45"
        allowedValues = ["15", "45", "480"]
        description   = "How long to run the recorder (min). 480 = 8h soak."
      }
      rotate_seconds = {
        type          = "String"
        default       = "600"
        allowedValues = ["60", "300", "600"]
        description   = "wslog rotation interval — the change under test."
      }
      sync_interval_minutes = {
        type          = "String"
        default       = "5"
        allowedValues = ["1", "5", "10"]
        description   = "S3 sync cadence."
      }
      release_key = {
        type        = "String"
        default     = "latest"
        description = "Release tarball key in the releases bucket (or 'latest')."
      }
      instance_type = {
        type        = "String"
        default     = var.default_instance_type
        description = "Keep t3.micro to match prod (OOM was size-specific)."
      }
      keep_on_failure = {
        type          = "String"
        default       = "false"
        allowedValues = ["false", "true"]
        description   = "Leave the box up for post-mortem if the run fails (reaper kills at TTL)."
      }
    }
    mainSteps = [
      {
        name   = "StartQaRun"
        action = "aws:executeAwsApi"
        inputs = {
          Service         = "stepfunctions"
          Api             = "StartExecution"
          StateMachineArn = aws_sfn_state_machine.qa.arn
          Name            = "qa-{{ automation:EXECUTION_ID }}"
          Input = jsonencode({
            run_id                = "{{ automation:EXECUTION_ID }}"
            duration_minutes      = "{{ duration_minutes }}"
            rotate_seconds        = "{{ rotate_seconds }}"
            sync_interval_minutes = "{{ sync_interval_minutes }}"
            release_key           = "{{ release_key }}"
            instance_type         = "{{ instance_type }}"
            keep_on_failure       = "{{ keep_on_failure }}"
          })
        }
      }
    ]
  })
}
