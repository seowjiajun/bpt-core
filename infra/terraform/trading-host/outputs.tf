output "instance_id" {
  description = "EC2 instance ID. Used for `aws ec2 stop-instances` / `start-instances` to pause billing without destroying state."
  value       = aws_instance.trading.id
}

output "public_ip" {
  description = "Stable Elastic IP. SSH here for bootstrap; once Tailscale is up, prefer the 100.x.y.z tailnet IP."
  value       = aws_eip.trading.public_ip
}

output "ssh_command" {
  description = "Convenience: copy/paste to SSH in. Assumes the matching private key is at ~/.ssh/id_ed25519."
  value       = "ssh -i ~/.ssh/id_ed25519 ubuntu@${aws_eip.trading.public_ip}"
}

output "data_volume_id" {
  description = "EBS volume ID for the data disk. Snapshot before risky operations; survives instance recreation."
  value       = aws_ebs_volume.data.id
}

output "stop_command" {
  description = "Pause billing (keeps EBS, EIP, SG): ~$3-5/mo idle cost. Compute charges resume on `start-instances`."
  value       = "aws ec2 stop-instances --instance-ids ${aws_instance.trading.id} --region ${var.aws_region}"
}

output "start_command" {
  description = "Resume the instance. Boot takes ~30 s; the persistent EBS at /opt/bpt is already populated."
  value       = "aws ec2 start-instances --instance-ids ${aws_instance.trading.id} --region ${var.aws_region}"
}

output "auto_stop_schedule" {
  description = "Daily auto-stop schedule details. Disable in tfvars for long-running tests."
  value = {
    name     = aws_scheduler_schedule.auto_stop.name
    cron     = aws_scheduler_schedule.auto_stop.schedule_expression
    state    = aws_scheduler_schedule.auto_stop.state
    timezone = aws_scheduler_schedule.auto_stop.schedule_expression_timezone
  }
}
