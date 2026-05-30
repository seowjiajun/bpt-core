"""Reaper for orphaned bpt-tape QA instances — layer 2 (independent backstop).

Step Functions guarantees teardown for normal runs, and the box self-terminates
at TTL. This daily sweep is the last line: it terminates any bpt_qa=true
instance older than MAX_AGE_MIN, covering boot failures (bootstrap never armed
the shutdown), cancelled shutdowns, or an SF failure mid-flight. Tag-scoped so
it can only ever touch QA boxes.
"""
import datetime
import os

import boto3

MAX_AGE_MIN = int(os.environ.get("MAX_AGE_MIN", "600"))  # 10h > 8h max run + buffer
ec2 = boto3.client("ec2")


def handler(event, context):
    now = datetime.datetime.now(datetime.timezone.utc)
    resp = ec2.describe_instances(Filters=[
        {"Name": "tag:bpt_qa", "Values": ["true"]},
        {"Name": "instance-state-name", "Values": ["pending", "running", "stopping", "stopped"]},
    ])
    stale = [
        inst["InstanceId"]
        for r in resp["Reservations"]
        for inst in r["Instances"]
        if (now - inst["LaunchTime"]).total_seconds() / 60 >= MAX_AGE_MIN
    ]
    if stale:
        ec2.terminate_instances(InstanceIds=stale)
    return {"terminated": stale, "max_age_min": MAX_AGE_MIN}
