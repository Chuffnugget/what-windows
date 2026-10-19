# Integration protocol contract

The Python files in `tests/integration_contract/` are exact copies from the sibling
WHAT integration, distributed under LGPL-3.0-or-later. Do not edit those copies
directly. Run `scripts/Sync-IntegrationContract.ps1` from the Windows repository
after intentional integration changes, then review and commit the changed files,
including `tests/integration_contract/source.json`.

The manifest hashes identify the tested source. Windows CI uses this copy without
private repository credentials. Local release builds with both checkouts compare
the current integration files against these hashes and reject drift. The release
gate runs `tests/integration_contract/tests/test_live_app.py`; the neighbouring
`test_protocol.py` supplies its package-loading fixture.
