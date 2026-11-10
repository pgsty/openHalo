# Security Policy

OpenHalo is an independent PostgreSQL 18 fork with the `aux_mysql` protocol
and SQL compatibility extension. This policy covers vulnerabilities in the
fork-specific kernel hooks, `aux_mysql`, release artifacts, and documentation.

## Reporting a Vulnerability

Please do not report security vulnerabilities through public GitHub issues,
discussions, pull requests, or social media.

Use one of these private channels instead:

1. **GitHub Private Vulnerability Reporting** (preferred):
   [Report a vulnerability](https://github.com/pgsty/openhalo/security/advisories/new)
   on the `pgsty/openhalo` repository.
2. **Email**: [ron@pgsty.com](mailto:ron@pgsty.com) or
   [rh@vonng.com](mailto:rh@vonng.com)

Include the affected release or commit, component, impact, reproduction steps
or proof of concept, and any required privileges or configuration. Redact
passwords, tokens, private keys, and customer data. Do not send database dumps
or complete configuration files unless requested through a private channel.

Please keep vulnerability details private until a fix, mitigation, or advisory
has been published. With your permission, we will credit you in the advisory
or release notes.

## Upstream Components

Report vulnerabilities in unmodified PostgreSQL code to the
[PostgreSQL Security team](https://www.postgresql.org/support/security/).
For issues that affect OpenHalo's changes, `aux_mysql`, or the combined
distribution, use the private channels above. If the boundary is unclear,
report it to OpenHalo and we will route it appropriately.

## Supported Releases

Security fixes are tracked on `master` and ship in the latest stable OpenHalo
18 release. Upgrade to the latest release to receive them; older release lines
are supported only when an advisory says otherwise.
