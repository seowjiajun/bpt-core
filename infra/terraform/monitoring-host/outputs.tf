output "instance_id" {
  description = "EC2 instance ID."
  value       = aws_instance.monitor.id
}

output "public_ip" {
  description = "Stable Elastic IP. SSH here for bootstrap; afterwards prefer the tailnet hostname."
  value       = aws_eip.monitor.public_ip
}

output "private_ip" {
  description = "Private IP inside the tape VPC. Used in scrape configs that originate from inside the same VPC."
  value       = aws_instance.monitor.private_ip
}

output "ssh_command" {
  description = "Convenience: copy/paste to SSH for bootstrap. Replace with `ssh ubuntu@bpt-monitor-tokyo-01` once Tailscale is enrolled."
  value       = "ssh -i ~/.ssh/id_ed25519 ubuntu@${aws_eip.monitor.public_ip}"
}

output "data_volume_id" {
  description = "EBS volume ID for the Prometheus tsdb. Snapshot before any major upgrade."
  value       = aws_ebs_volume.data.id
}

output "tape_host_private_ip" {
  description = "Private IP of the tape host. Pass this to bootstrap.sh as the scrape target so prometheus.yml hits the recorder over the VPC."
  value       = data.aws_instance.tape.private_ip
}

output "tape_host_instance_id" {
  description = "Tape host instance ID — convenience output, useful for cross-referencing CloudWatch / SSM."
  value       = data.aws_instance.tape.id
}
