kgt-parity -- vendored parity-snapshot ABI header
=================================================

Source:  kgtengine (the author's cross-engine ground-truth toolkit), header
         include/kgt/kgt_parity_snapshot.h.
License: Apache-2.0 (SPDX tag carried in FM2KHook/src/parity/parity_recorder.cpp).

Why vendored
------------
FM2KHook/src/parity/parity_recorder.cpp is the ONLY consumer and it needs only
this ONE header (it #includes <kgt/kgt_parity_snapshot.h>, which itself pulls in
nothing but <stdint.h>). The old FM2KHook/CMakeLists.txt added an include path
into a sibling repo (../../kgtengine/include) that does not exist in a fresh
clone, so the hook DLL could not compile for anyone who didn't also have the
kgtengine checkout next to wanwan. Copying the single self-contained header in-
tree makes the hook build self-contained.

Updating
--------
If kgtengine's parity-snapshot struct ABI changes, re-copy
include/kgt/kgt_parity_snapshot.h over vendored/kgt-parity/include/kgt/. Keep it
in lock-step with kgtengine's recorder/diff tool so the snapshots stay readable
by that tool.
