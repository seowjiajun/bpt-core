# Lambda container image for bpt-secmaster-refresh.
#
# Build:    docker build -t bpt-secmaster-refresh:latest .
# Local run: docker run --rm \
#              -e SECMASTER_DSN=postgres://host.docker.internal/secmaster_dev \
#              bpt-secmaster-refresh:latest handler.lambda_handler
#
# Push to ECR (phase 4):
#   aws ecr get-login-password | docker login --username AWS --password-stdin <acct>.dkr.ecr.ap-northeast-1.amazonaws.com
#   docker tag bpt-secmaster-refresh:latest <acct>.dkr.ecr.ap-northeast-1.amazonaws.com/bpt-secmaster-refresh:latest
#   docker push <acct>.dkr.ecr.ap-northeast-1.amazonaws.com/bpt-secmaster-refresh:latest

FROM public.ecr.aws/lambda/python:3.12

# Install OS-level deps. psycopg[binary] includes libpq via wheel so we
# don't need postgresql-devel here.

COPY requirements.txt ${LAMBDA_TASK_ROOT}/
RUN pip install --no-cache-dir -r ${LAMBDA_TASK_ROOT}/requirements.txt

# Copy application code. Flat layout — handler.py at the top, venues/
# beside it. Lambda's default WORKDIR is /var/task = $LAMBDA_TASK_ROOT.
COPY handler.py db.py symbology.py render.py ${LAMBDA_TASK_ROOT}/
COPY venues/ ${LAMBDA_TASK_ROOT}/venues/

# Lambda runtime entrypoint. Image's base ENTRYPOINT is already
# /var/runtime/bootstrap; we just supply the CMD = "handler.lambda_handler".
CMD ["handler.lambda_handler"]
