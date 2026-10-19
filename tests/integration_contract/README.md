# Integration protocol contract

Exact copies from the sibling WHAT integration, distributed under the repository GPL license. Do not edit these Python files here. Run `scripts/Sync-IntegrationContract.ps1` from the Windows repository after intentional integration changes, then review and commit the changes, including `source.json`.

The manifest hashes identify the tested source. Windows CI uses this copy without private repository credentials. Local release builds with both checkouts compare the current integration files against these hashes and reject drift. Only `tests/test_live_app.py` is executed by the release gate; `test_protocol.py` supplies its package-loading fixture.
