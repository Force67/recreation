# tests/upstream

Regression checks over contracts recreation depends on but does not own. The
code under test lives in the sibling `rx` and `equilibrium` repos, so these
cannot sit beside it; they pin the behaviour recreation relies on and fail here
first when an upstream change breaks it.
