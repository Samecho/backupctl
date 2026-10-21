# Temporary-session cloud preparation validation

Revision `ac7879eb6606260a8c6649d7b4f83d7e1c66b77e` passed
[Linux CI run 39518475515](https://github.com/Samecho/backupctl/actions/runs/39518475515).
The saved CTest logs contain 42 tests per configuration: nine unit tests, eight
integration tests, seven fault tests, six security tests, seven cloud CI guard
tests, and five SDK HTTPS fixture tests. GCC release, Clang, ASan with UBSan,
and TSan all passed.

The new fixture case runs the bounded acceptance harness using temporary session
credentials and all four snapshots. It checks the credential source and fixture
label, preserves source inventories for later reader checks, and checks that
credentials do not appear in the command journal. The guard tests reject unsafe
cleanup destinations, incomplete or oversized listings, endpoint overrides, and
false IAM passes caused by timeouts or expired credentials. They also check that
cleanup verifies the prefix is empty before recording success.

`infra/s3-acceptance.json` passed cfn-lint 1.57.2 for `ca-central-1` with exit code
zero. `.github/workflows/s3-acceptance.yml` passed actionlint 1.7.12 with exit
code zero. Both tools ran in disposable containers. No Windows packages,
profiles, persistent credentials, or global settings were added.

The template and workflow hashes are in `provenance.json`. The workflow is
registered in GitHub and can only be dispatched manually. Its AWS steps have
not run: the account still needs registration, a reviewed CloudFormation stack,
and the bucket and role variables. These logs validate preparation and local
SDK behavior; they are not real S3 or IAM acceptance evidence.
