# PGO helper failure and retry guards

Run this source-only suite with Python 3.11 or newer:

```sh
python -B tests/pgo-helper-tests/test_pgo_helper.py
```

Six tests use authored byte strings and mocked `pgomgr`/LINK operations in
temporary external scratch. They exercise a successful merge/link, growing
or newly added PGCs, a partial failing merge, profile-identity rejection and
refusal to retry an older unrecorded merge. Failed attempt outputs remain
unchanged, retries start from the unmerged training PGD, and a successful
result pins the immutable merged input separately from LINK's mutable PGD.

No compiler, MSVC tool, game, captured profile or generated catalog is
required. This tests the helper's transaction and provenance guards; it
does not validate real MSVC profile generation or gameplay performance.
Package sources are GPL-3.0-or-later; see [the tests license](../LICENSE).
