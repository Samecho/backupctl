# S3 acceptance resources

`s3-acceptance.json` creates one private test bucket and three GitHub OIDC roles.
It does not create an AWS account, upgrade a plan, create access keys, or change
anything on the development computer. It has not been deployed or accepted
against AWS yet.

The bucket uses SSE-S3, disables ACLs, blocks all public access, and rejects HTTP.
Every role can list only `backupctl/test` and its children. The writer can read
and put objects there. The reader can only read, and the cleanup role can only
delete. None can manage the bucket, use KMS, or access other AWS services.

Each role trusts only the exact `main` branch subject from this repository:

```text
repo:Samecho@78562835/backupctl@1426593746:ref:refs/heads/main
```

This repository uses GitHub's immutable subject format. Check
`repos/Samecho/backupctl/actions/oidc/customization/sub` before deploying if the
repository is renamed or moved. Do not replace the subject with a wildcard.
The roles allow sessions of up to one hour; the acceptance workflow should
request 15 minutes. Credentials belong only to the runner process.

## Create the stack

Finish the AWS registration and confirm that the account uses the Free plan.
Eligible new accounts on that plan do not incur usage charges while the plan
is active. AWS can request payment details and temporarily hold USD 1 or the
local equivalent to verify a card. Do not select a Paid plan or join an AWS
Organization for this test. Review the actual plan shown for the account before
creating resources.

In CloudFormation, select `ca-central-1`, create a standard stack with new
resources, and upload `s3-acceptance.json`. Use `backupctl-acceptance` as the
stack name. Leave `ExistingGitHubProviderArn` empty for a new account. If a
GitHub OIDC provider already exists, supply its ARN instead of creating another.
Review the resources and IAM changes before creating the stack. No named IAM
roles or administrative runner permissions are required.

Save the outputs: `Bucket`, `Region`, `Prefix`, `WriterRoleArn`, `ReaderRoleArn`,
and `CleanupRoleArn`. These identifiers are configuration, not credentials.
They can be stored as GitHub repository variables. Do not store AWS keys or a
root password in the repository or GitHub secrets for this workflow.

## Keep the test disposable

Use only the small synthetic acceptance input. The 50 GiB performance run is a
local test and should not be uploaded to AWS as part of this acceptance.
S3 storage, requests, and network transfer have usage prices; free credits or
allowances depend on the account. A billing alert is not a spending cap.

The workflow should remove only its own `acceptance-<uuid>/` objects after saving
evidence. A seven-day expiration rule under `backupctl/test/` is a fallback for
interrupted tests. That entire prefix is disposable: never use it for a backup
you intend to keep. The template does not enable versioning, replication,
acceleration, inventory, access logs, or paid monitoring features.

Deleting the stack removes its IAM roles and a provider created by this stack.
The bucket and its TLS policy are retained so a stack deletion cannot erase
backup data. After inspecting the saved evidence, empty and delete this dedicated
bucket separately if it is no longer needed. Reuse an existing OIDC provider if
other projects depend on it.

References: [AWS Free plan](https://docs.aws.amazon.com/awsaccountbilling/latest/aboutv2/free-tier-plans.html),
[signup and card verification](https://docs.aws.amazon.com/accounts/latest/reference/sign-in-new.html),
[S3 pricing](https://aws.amazon.com/s3/pricing/),
[GitHub OIDC subjects](https://docs.github.com/en/actions/reference/security/oidc),
[CloudFormation OIDC provider](https://docs.aws.amazon.com/AWSCloudFormation/latest/TemplateReference/aws-resource-iam-oidcprovider.html).
