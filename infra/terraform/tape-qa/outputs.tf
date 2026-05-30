data "aws_region" "current" {}

output "qa_bucket" {
  description = "QA results bucket (per-run prefixes, lifecycle-expired)."
  value       = aws_s3_bucket.qa.id
}

output "state_machine_arn" {
  description = "Step Functions state machine that orchestrates a QA run."
  value       = aws_sfn_state_machine.qa.arn
}

output "ssm_document_name" {
  description = "SSM Automation document — the parameterized 'form' to trigger a run."
  value       = aws_ssm_document.qa.name
}

output "trigger_form_url" {
  description = "Deep link to the Execute-automation form (the click-UI)."
  value       = "https://${data.aws_region.current.name}.console.aws.amazon.com/systems-manager/automation/execute/${aws_ssm_document.qa.name}?region=${data.aws_region.current.name}"
}

output "execution_console_url" {
  description = "Deep link to the state machine — watch the live execution graph."
  value       = "https://${data.aws_region.current.name}.console.aws.amazon.com/states/home?region=${data.aws_region.current.name}#/statemachines/view/${aws_sfn_state_machine.qa.arn}"
}

output "sns_topic_arn" {
  description = "Confirm the email subscription on this topic after first apply."
  value       = aws_sns_topic.qa.arn
}
