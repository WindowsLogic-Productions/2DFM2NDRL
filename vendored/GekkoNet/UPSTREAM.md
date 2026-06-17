GekkoNet -- vendored in-tree (flattened from a submodule on 2026-06-17)
=======================================================================

Upstream:  https://github.com/HeatXD/GekkoNet
Base commit (last upstream commit we branched from):
    8ca4058  Add runahead support to GameSession (#45)

Why this is flattened (committed as plain files) instead of a git submodule
---------------------------------------------------------------------------
This tree carries 3 local commits on top of upstream 8ca4058 that exist on NO
public remote -- a fresh `git submodule update` could never fetch them, so the
old submodule pin (9e973c0) made the repo un-cloneable for anyone but the
original author's machine. Flattening the source in-tree guarantees the repo
builds from a plain `git clone`.

Local patches on top of 8ca4058 (newest first)
-----------------------------------------------
    9e973c0  Expose gekko_confirmed_frame(): highest frame with REAL inputs
             from all players (never predicted) -- lets the host gate
             replay/spectator recording on confirmed-only inputs.
    6fa37f1  Late spectator-join support: configurable input_history_size +
             non-gating spectator handshake.
    7b62a70  backend: silently drop magic-mismatched packets during the sync
             handshake.

Updating GekkoNet in the future
-------------------------------
There is no submodule to `git submodule update` anymore. To pull upstream
changes, fetch HeatXD/GekkoNet, rebase/replay the 3 patches above onto the new
upstream tip, and copy GekkoLib/{src,include,thirdparty} back over this tree.
Only GekkoLib/ is compiled (see the root CMakeLists.txt add_library(GekkoNet
STATIC ...) and the GekkoLib include dirs); Examples/docs/.sln were dropped.

License: see ./LICENSE (unchanged from upstream).
